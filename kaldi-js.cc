// online2bin/online2-wav-nnet3-latgen-faster.cc

// Copyright 2014  Johns Hopkins University (author: Daniel Povey)
//           2016  Api.ai (Author: Ilya Platonov)

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "kaldi-wa/src/feat/resample.h"
#include "kaldi-wa/src/feat/wave-reader.h"
#include "kaldi-wa/src/fstext/fstext-lib.h"
#include "kaldi-wa/src/lat/lattice-functions.h"
#include "kaldi-wa/src/nnet3/nnet-utils.h"
#include "kaldi-wa/src/online2/online-nnet3-decoding.h"
#include "kaldi-wa/src/online2/online-nnet2-feature-pipeline.h"
#include "kaldi-wa/src/online2/onlinebin-util.h"
#include "kaldi-wa/src/online2/online-timing.h"
#include "kaldi-wa/src/online2/online-endpoint.h"
#include "kaldi-js.pb.h"

#include <algorithm>
#include <sys/types.h>
#include <math.h>

#include <sstream>
#include <string>
#include <iostream>

#include <emscripten.h>

using namespace kaldi;

string GetBestTranscript(const fst::SymbolTable *word_syms,
                    const LatticeFasterOnlineDecoder &decoder) {
    if (!decoder.NumFramesDecoded()) {
        KALDI_WARN << "No frames decoded.";
        return "";
    }

    Lattice best_path_lat;
    decoder.GetBestPath(&best_path_lat);

    if (best_path_lat.NumStates() == 0) {
        KALDI_WARN << "Empty lattice.";
        return "";
    }

    LatticeWeight weight;
    std::vector<int32> alignment;
    std::vector<int32> words;
    GetLinearSymbolSequence(best_path_lat, &alignment, &words, &weight);

    std::ostringstream os;
    for (size_t i = 0; i < words.size(); i++) {
        std::string s = word_syms->Find(words[i]);
        os << s;
        if (i < words.size() - 1)
            os << " ";
        if (s == "")
            KALDI_ERR << "Word-id " << words[i] << " not in symbol table.";
    }

    return os.str();
}

class Asr {
public:
    ~Asr();
    void DecodeChunk();
    void ResetDecoder();
    // feature_opts includes configuration for the iVector adaptation,
    // as well as the basic features.
    OnlineNnet2FeaturePipelineConfig feature_opts;
    nnet3::NnetSimpleLoopedComputationOptions decodable_opts;
    LatticeFasterDecoderConfig decoder_opts;
    OnlineEndpointConfig endpoint_opts;
    OnlineNnet2FeaturePipelineInfo *feature_info;
    TransitionModel trans_model;
    nnet3::AmNnetSimple am_nnet;
    // this object contains precomputed stuff that is used by all decodable
    // objects.  It takes a pointer to am_nnet because if it has iVectors it has
    // to modify the nnet to accept iVectors at intervals.
    nnet3::DecodableNnetSimpleLoopedInfo *decodable_info;
    fst::Fst<fst::StdArc> *decode_fst;
    fst::SymbolTable *word_syms;
    int64 last_output;
    OnlineTimingStats timing_stats;
    LinearResample *resampler;
    OnlineNnet2FeaturePipeline *feature_pipeline;
    OnlineIvectorExtractorAdaptationState *adaptation_state;
    OnlineSilenceWeighting *silence_weighting;
    SingleUtteranceNnet3Decoder *decoder;
    OnlineTimer *decoding_timer;
    BaseFloat in_samp_freq;
    BaseFloat asr_samp_freq;
    std::vector<std::pair<int32, BaseFloat> > delta_weights;
    Vector<BaseFloat> chunk;
    int chunk_valid;
    int total_length;
    bool enabled;
    bool endpoint_detected;
    ServerMessage message;
};
static Asr *asr;

Asr::~Asr() {
    delete decoding_timer;
    delete decoder;
    delete silence_weighting;
    delete adaptation_state;
    delete feature_pipeline;
    delete word_syms;
    delete decode_fst;
    delete decodable_info;
    delete feature_info;
}

void init(ClientMessage &message) {
    using namespace kaldi;
    using namespace fst;

    typedef kaldi::int32 int32;
    typedef kaldi::int64 int64;

    asr = new Asr();

    const char *usage =
            "Performs online decoding with neural nets\n"
            "(nnet3 setup), with optional iVector-based speaker adaptation and\n"
            "optional endpointing.  Note: some configuration values and inputs are\n"
            "set via config files whose filenames are passed as options\n"
            "\n"
            "Usage: online2-wav-nnet3-latgen-faster [options] <nnet3-in> <fst-in>\n";

    ParseOptions po(usage);

    std::string word_syms_rxfilename;
    BaseFloat chunk_length_secs = 0.18;
    bool do_endpointing = false;

    po.Register("chunk-length", &chunk_length_secs,
                            "Length of chunk size in seconds, that we process.  Set to <= 0 "
                            "to use all input in one chunk.");
    po.Register("word-symbol-table", &word_syms_rxfilename,
                            "Symbol table for words [for debug output]");
    po.Register("do-endpointing", &do_endpointing,
                            "If true, apply endpoint detection");

    asr->feature_opts.Register(&po);
    asr->decodable_opts.Register(&po);
    asr->decoder_opts.Register(&po);
    asr->endpoint_opts.Register(&po);

    kaldi::Init &init = *message.mutable_init();
    int argc = init.args_size() + 1;
    const char **argv = new const char *[argc];
    argv[0] = "";  // placeholder for the command binary
    for (int i = 1; i < argc; i++) {
        argv[i] = init.args(i - 1).c_str();
    }
    po.Read(argc, argv);
    delete[] argv;

    if (po.NumArgs() != 2) {
        po.PrintUsage();
        return;
    }

    std::string nnet3_rxfilename = po.GetArg(1),
            fst_rxfilename = po.GetArg(2);

    {
        bool binary;
        Input ki(nnet3_rxfilename, &binary);
        asr->trans_model.Read(ki.Stream(), binary);
        asr->am_nnet.Read(ki.Stream(), binary);
        SetBatchnormTestMode(true, &(asr->am_nnet.GetNnet()));
        SetDropoutTestMode(true, &(asr->am_nnet.GetNnet()));
    }

    asr->decodable_info = new nnet3::DecodableNnetSimpleLoopedInfo(asr->decodable_opts, &asr->am_nnet);

    std::ifstream ifs;
    ifs.open(fst_rxfilename, std::ifstream::binary);
    fst::FstReadOptions ropts(fst_rxfilename);
    ropts.source = fst_rxfilename;
    ropts.mode = fst::FstReadOptions::MAP;
    asr->decode_fst = fst::Fst<fst::StdArc>::Read(ifs, ropts);
    if (!asr->decode_fst) {
        KALDI_ERR << "failed to load decode graph";
    }

    if (word_syms_rxfilename != "")
        if (!(asr->word_syms = fst::SymbolTable::ReadText(word_syms_rxfilename)))
            KALDI_ERR << "Could not read symbol table from file "
                                << word_syms_rxfilename;

    //asr_proto::ServerMessage result_message;
    //asr_proto::Result &result = *result_message.mutable_result();

    /*
    if (start_msg.adaptation_state().size() > 0) {
        KALDI_LOG << "loading adaptation state";
        std::istringstream adaptation_state_str(start_msg.adaptation_state());
        asr->adaptation_state.Read(adaptation_state_str, false);
    }
    */

    //asr->feature_info->mfcc_opts.frame_opts.samp_freq = 8000; //message.init().sample_frequency();
    //asr->feature_info->mfcc_opts.mel_opts.high_freq = 4000;  // should actually be 3800 like in mfcc.conf


    asr->in_samp_freq = message.init().sample_frequency();

    asr->ResetDecoder();

    int chunk_length = int32(asr->asr_samp_freq * chunk_length_secs);
    // XXX check validity ^
    if (chunk_length == 0)
        chunk_length = 1;

    asr->chunk.Resize(chunk_length);
    KALDI_LOG << "started!";

    EM_ASM(postMessage('worker-ready'););
}

void start() {
    asr->enabled = true;
}

void Asr::ResetDecoder() {
    if (this->decoder) {
        delete this->decoding_timer;
        this->decoding_timer = NULL;
        delete this->decoder;
        this->decoder = NULL;
        delete this->resampler;
        this->resampler = NULL;
        delete this->silence_weighting;
        delete this->feature_pipeline;
        delete this->adaptation_state;
        delete this->feature_info;
    }
    this->chunk_valid = 0;
    asr->last_output = 0;
    asr->feature_info = new OnlineNnet2FeaturePipelineInfo(asr->feature_opts);
    asr->asr_samp_freq = asr->feature_info->mfcc_opts.frame_opts.samp_freq;
    asr->adaptation_state = new OnlineIvectorExtractorAdaptationState(asr->feature_info->ivector_extractor_info);
    asr->feature_pipeline = new OnlineNnet2FeaturePipeline(*asr->feature_info);
    asr->feature_pipeline->SetAdaptationState(*asr->adaptation_state);
    asr->silence_weighting = new OnlineSilenceWeighting(asr->trans_model, asr->feature_info->silence_weighting_config, asr->decodable_opts.frame_subsampling_factor);
    this->resampler = new LinearResample(this->in_samp_freq, this->asr_samp_freq, 3900, 4);
    this->decoder = new SingleUtteranceNnet3Decoder(this->decoder_opts, this->trans_model,
                                        *this->decodable_info,
                                        *this->decode_fst, this->feature_pipeline);
    this->decoding_timer = new OnlineTimer("utt");
    this->endpoint_detected = false;
    this->enabled = false;
}

void process_audio(ClientMessage &message) {
    if (!asr->enabled) {
        return;
    }

    Vector<BaseFloat> input(message.audio().audio_size());
    for (int i = 0; i < input.Dim(); i++) {
        input(i) = message.audio().audio(i);
    }

    Vector<BaseFloat> resampled;
    asr->resampler->Resample(input, false, &resampled);
    resampled.Scale(kWaveSampleMax);

    int new_i = 0;
    while (new_i < resampled.Dim() && !asr->endpoint_detected) {
        int copy_len = std::min(asr->chunk.Dim() - asr->chunk_valid, resampled.Dim() - new_i);
        for (int i = 0; i < copy_len; i++) {
            asr->chunk(asr->chunk_valid + i) = resampled(new_i + i);
        }
        new_i += copy_len;
        asr->chunk_valid += copy_len;

        if (asr->chunk_valid == asr->chunk.Dim()) {
            // we have a whole chunk. decode.
            asr->DecodeChunk();
        }
    }

    if (asr->endpoint_detected) {
        std::string message;
        if (asr->decoder->Decoder().FinalRelativeCost() <= 8) {
            message = GetBestTranscript(asr->word_syms, asr->decoder->Decoder());
        } else {
            message = "";
        }
        /*
        AsrResult *result = asr->message.mutable_asr_result();
        result->Clear();
        result->set_transcript(GetBestTranscript(asr->word_syms, asr->decoder->Decoder()));
        result->set_final_relative_cost(asr->decoder->Decoder().FinalRelativeCost());
        result->set_endpoint_detected(asr->endpoint_detected);
        result->set_last(asr->endpoint_detected);
        std::string message;
        result->SerializeToString(&message);
        */

        EM_ASM_({
            postByteArray($0, $1);
        }, message.c_str(), message.length());
        asr->enabled = false; // stop accepting audio but don't reset until the client tells us to
    }
}

void Asr::DecodeChunk() {
    int this_chunk_length = this->chunk_valid;
    asr->total_length += this_chunk_length;

    asr->feature_pipeline->AcceptWaveform(asr->asr_samp_freq, this->chunk.Range(0, this_chunk_length));
    this->chunk_valid = 0;

    asr->decoding_timer->WaitUntil(asr->total_length / asr->asr_samp_freq);
    //if (this_chunk_length != asr->chunk.Dim())
     //   asr->feature_pipeline->InputFinished();

    if (asr->silence_weighting->Active() && asr->feature_pipeline->IvectorFeature() != NULL) {
        asr->silence_weighting->ComputeCurrentTraceback(asr->decoder->Decoder());
        asr->silence_weighting->GetDeltaWeights(asr->feature_pipeline->NumFramesReady(), &delta_weights);
        asr->feature_pipeline->IvectorFeature()->UpdateFrameWeights(delta_weights);
    }

    asr->decoder->AdvanceDecoding();

    // send an ASRResult approx once per second
    if (asr->total_length - asr->last_output > asr->asr_samp_freq) {
        asr->endpoint_detected = asr->decoder->EndpointDetected(asr->endpoint_opts);

        KALDI_LOG << asr->decoder->Decoder().FinalRelativeCost() << ": " << GetBestTranscript(asr->word_syms, asr->decoder->Decoder());

        // might be good to look at this stuff later for more confidence information
        //asr->decoder.FinalizeDecoding();  // needed (I think) to generate a lattice

        // In an application you might avoid updating the adaptation state if
        // you felt the utterance had low confidence.  See lat/confidence.h
        //asr->feature_pipeline.GetAdaptationState(&asr->adaptation_state);

        if (false && asr->endpoint_detected) {
            std::ostringstream adaptation_state_str;
            asr->feature_pipeline->GetAdaptationState(asr->adaptation_state);
            asr->adaptation_state->Write(adaptation_state_str, false);
            //result.set_adaptation_state(adaptation_state_str.str());
        }

        asr->last_output += asr->asr_samp_freq;
    }
}

void WriteMessage(const ServerMessage &message) {
    std::string message_str;
    message.SerializeToString(&message_str);
}

extern "C" {
    void EMSCRIPTEN_KEEPALIVE handle_message(uint8_t *message_bytes, int len) {
        std::string message_str((char *)message_bytes, len);
        kaldi::ClientMessage message;
        if (!message.ParseFromString(message_str)) {
            KALDI_LOG << "failed to parse";
        } else {
            if (message.has_init()) {
                init(message);
            } else if (message.has_start()) {
                asr->enabled = true;
            } else if (message.has_audio()) {
                process_audio(message);
            } else if (message.has_cancel()) {
                //asr->decoding_timer->OutputStats(&asr->timing_stats);
                //asr->timing_stats.Print(true);
                asr->ResetDecoder();
            } else {
                KALDI_LOG << "unhandled message type";
            }
        }
    }

    int EMSCRIPTEN_KEEPALIVE main() {
        EM_ASM(
            importScripts('kaldi-worker.js');
        );

        return 0;
    }
}
