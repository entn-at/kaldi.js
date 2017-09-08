#!/usr/bin/env python3

import argparse
import glob
import itertools
import io
import json
import os
import pprint
import re
import subprocess
import sys

DIR = os.path.dirname(__file__)
KALDI_WSJ = os.path.join(DIR, 'kaldi', 'egs', 'wsj', 's5')
KALDI_ENV = json.loads(subprocess.check_output(". ./path.sh; python -c 'import os, json; print json.dumps(dict(os.environ))'", shell=True, cwd=KALDI_WSJ))


class Type(object):
    def __init__(self, name, values):
        self.name = name
        self.values = values


class Intent(object):
    def __init__(self, name, slot_jsons=None):
        self.name = name
        self.slots = {}
        if slot_jsons is not None:
            self.slots = {s['name']: s['type'] for s in slot_jsons}
        self.utts = []

    def __repr__(self):
        return '{}: {}, {}'.format(self.name, self.utts, self.slots)


def compile_fst(src):
    print(len(src))
    cmd = 'fstcompile | fstarcsort --sort_type=ilabel'
    compiler = subprocess.Popen(cmd, shell=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=KALDI_ENV)
    fst, stderr = compiler.communicate(src.encode('latin-1'))  # input is latin-1 because KALDI_ENV has LC_ALL=C. output is binary
    if compiler.returncode:
        print(stderr)
        raise subprocess.CalledProcessError(compiler.returncode, cmd)
    print(len(fst))
    return fst


def main():
    parser = argparse.ArgumentParser(description='Make a Kaldi decode graph for an Alexa skill')
    parser.add_argument('skill', help='path to Alexa skill')
    parser.add_argument('lang', help='path to Kaldi lang directory')
    parser.add_argument('--model', help='Kaldi model directory. Defaults to the directory above the graph')
    parser.add_argument('graph', help='path to write the graph')
    args = parser.parse_args()

    skill_dir = args.skill
    lang_dir = args.lang
    graph_dir = args.graph
    if args.model is None:
        model_dir = os.path.join(graph_dir, os.pardir)
    else:
        model_dir = args.model

    words = {}
    for word_wid in open(os.path.join(lang_dir, 'words.txt'), encoding="latin-1"):
        word, wid = word_wid.split()
        words[word] = int(wid)
    word_ids = itertools.count(max(words.values()) + 1)

    types = {}
    for type_path in glob.glob(os.path.join(skill_dir, 'alexa-types', '*')):
        name = os.path.basename(type_path)
        values = [value.strip().lower() for value in open(type_path)]
        types[name] = Type(name, values)

    print(types)

    intents_json = json.load(open(os.path.join(skill_dir, 'intents.json')))
    intents = {}
    for intent_json in intents_json['intents']:
        name = intent_json['intent']
        slot_jsons = intent_json.get('slots')
        intents[name] = Intent(name, slot_jsons)

    for utt_line in open(os.path.join(skill_dir, 'utterences.txt')):
        print(utt_line.strip())
        intent_name, utt = utt_line.split(None, 1)
        intents[intent_name].utts.append(utt.strip())

    pprint.pprint(intents)

    fst_src = io.StringIO()

    eps = words['<eps>']
    unk = words['<unk>']
    restart = words['[restart]'] = next(word_ids)

    states = itertools.count()
    initial = next(states)
    final = next(states)
    print(initial, initial, unk, restart, 0, file=fst_src)
    print(final, 0, file=fst_src)
    for intent in intents.values():
        intent_word = '[{}]'.format(intent.name)
        assert intent_word not in words, intent_word
        words[intent_word] = next(word_ids)

        intent_initial = next(states)
        print(initial, intent_initial, eps, words[intent_word], 0, file=fst_src)
        for utt in intent.utts:
            utt_tokens = utt.split()
            utt_token_states = [intent_initial]
            for token in utt_tokens[:-1]:
                state = next(states)
                utt_token_states.append(state)
                print(state, initial, unk, restart, 0, file=fst_src)
            utt_token_states.append(final)

            for i, token in enumerate(utt_tokens):
                token_prev_state = utt_token_states[i]
                token_next_state = utt_token_states[i+1]
                if token[0] == '{' and token[-1] == '}':
                    token_type = types[intent.slots[token[1:-1]]]
                    for value in token_type.values:
                        value_tokens = re.split('[ -]', value)
                        value_token_states = [token_prev_state]
                        for value_token in value_tokens[:-1]:
                            state = next(states)
                            value_token_states.append(state)
                            print(state, initial, unk, restart, 0, file=fst_src)
                        value_token_states.append(token_next_state)

                        for j, value_token in enumerate(value_tokens):
                            value_token_prev_state = value_token_states[j]
                            value_token_next_state = value_token_states[j + 1]
                            if value_token in words:
                                wid = words[value_token]
                                print(value_token_prev_state, value_token_next_state, wid, wid, 0, file=fst_src)
                            else:
                                print(value_token, 'is unk (value)')
                                print(value_token_prev_state, value_token_next_state, unk, unk, 0, file=fst_src)

                elif token in words:
                    wid = words[token]
                    print(token_prev_state, token_next_state, wid, wid, 0, file=fst_src)
                else:
                    print(token, 'is unk')
                    print(token_prev_state, token_next_state, unk, unk, 0, file=fst_src)

    G_fst = os.path.join(lang_dir, 'G.fst')
    with open(G_fst, 'wb') as fst:
        fst.write(compile_fst(fst_src.getvalue()))

    print('building decode graph')
    subprocess.check_call('mkgraph.sh --self-loop-scale 1.0 {lang_dir} {model_dir} {graph_dir}'.format(lang_dir=lang_dir, model_dir=model_dir, graph_dir=graph_dir), shell=True, env=KALDI_ENV)
    with open(os.path.join(graph_dir, 'words.txt'), 'w', encoding='latin-1') as word_file:
        for word, wid in sorted(words.items(), key=lambda word_wid: word_wid[1]):
            print(word, wid, file=word_file)


if __name__ == '__main__':
    main()
