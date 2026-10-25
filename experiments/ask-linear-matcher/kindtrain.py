#!/usr/bin/env python3
"""Train the kind matcher's linear model from data/kinds/phrasings.tsv and write data/kinds/weights.bin.

Features: word unigrams and character 3-5-grams of each word (space-padded), both tf-idf weighted
(1 + ln tf, smoothed idf) and unit-length per block; a multinomial logistic regression over them gives
one probability per kind. Chat-speak and synonym tables are applied first and stored in the file, so
the C++ side (src/core/kinds.cpp) tokenizes and normalizes the same way.

  kindtrain.py REPO_ROOT [--eval]     --eval also reports 5-fold CV and the lands() questions of test_core.cpp
"""
import math, os, re, struct, sys, collections
import numpy as np
from scipy.sparse import csr_matrix, hstack
from sklearn.linear_model import LogisticRegression

NGRAM = (3, 5)
MIN_DF = 2
PRUNE = 0.02   # drop a feature when no kind weights it more than this (in absolute value)
C = 20.0


def lower_ascii(s):
    return "".join(chr(ord(c) + 32) if "A" <= c <= "Z" else c for c in s)


def tokens(s):
    return re.findall(r"[a-z0-9%]+", lower_ascii(s))


def load_table(path, sep):
    t = {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or sep not in line:
            continue
        a, b = line.split(sep, 1)
        if sep == ":":
            for w in b.split():
                t[w] = a.strip()
        else:
            t[a.strip()] = b.strip()
    return t


def normalize(words, chat, syn):
    out = []
    for w in words:
        for piece in chat.get(w, w).split():
            out.append(syn.get(piece, piece))
    return out


def char_grams(word):
    padded = " " + word + " "
    for n in range(NGRAM[0], NGRAM[1] + 1):
        for i in range(0, len(padded) - n + 1):
            yield padded[i:i + n]


def features(question, chat, syn):
    """Two bags: word counts and char-gram counts."""
    words = normalize(tokens(question), chat, syn)
    w = collections.Counter(words)
    c = collections.Counter(g for word in words for g in char_grams(word))
    return w, c


class Vocab:
    def __init__(self, docs, min_df):
        df = collections.Counter()
        for d in docs:
            df.update(d.keys())
        self.index = {f: i for i, f in enumerate(sorted(f for f, n in df.items() if n >= min_df))}
        N = len(docs)
        self.idf = np.array([math.log((1 + N) / (1 + df[f])) + 1 for f in self.index], dtype=np.float32)

    def matrix(self, docs):
        rows, cols, vals = [], [], []
        for r, d in enumerate(docs):
            v = [(self.index[f], (1 + math.log(n)) * self.idf[self.index[f]]) for f, n in d.items() if f in self.index]
            norm = math.sqrt(sum(x * x for _, x in v)) or 1.0
            for j, x in v:
                rows.append(r); cols.append(j); vals.append(x / norm)
        return csr_matrix((vals, (rows, cols)), shape=(len(docs), len(self.index)), dtype=np.float32)


def write_weights(path, kinds, bias, wv, cv, W, chat, syn):
    def s(x):
        b = x.encode("utf-8"); return struct.pack("<H", len(b)) + b
    with open(path, "wb") as f:
        f.write(b"ASKLIN1\0")
        f.write(struct.pack("<iiii", len(kinds), W.shape[0], len(chat), len(syn)))
        for k, b in zip(kinds, bias):
            f.write(s(k) + struct.pack("<f", b))
        for feat, idf in zip(wv.index, wv.idf):
            f.write(b"\0" + s(feat) + struct.pack("<f", idf))
        for feat, idf in zip(cv.index, cv.idf):
            f.write(b"\1" + s(feat) + struct.pack("<f", idf))
        f.write(W.astype("<f4").tobytes())
        for a, b in sorted(chat.items()):
            f.write(s(a) + s(b))
        for a, b in sorted(syn.items()):
            f.write(s(a) + s(b))


def main():
    root = sys.argv[1]
    do_eval = "--eval" in sys.argv
    chat = load_table(os.path.join(root, "vendor/unllm/data/chatspeak.txt"), "->")
    syn = load_table(os.path.join(root, "vendor/unllm/data/synonyms.txt"), ":")
    X, y = [], []
    for line in open(os.path.join(root, "data/kinds/phrasings.tsv"), encoding="utf-8"):
        line = line.rstrip("\n")
        if "\t" in line:
            k, p = line.split("\t", 1); X.append(p); y.append(k)
    y = np.array(y)
    kinds = sorted(set(y))

    def fit(Xtr, ytr):
        docs = [features(q, chat, syn) for q in Xtr]
        wv = Vocab([d[0] for d in docs], MIN_DF); cv = Vocab([d[1] for d in docs], MIN_DF)
        M = hstack([wv.matrix([d[0] for d in docs]), cv.matrix([d[1] for d in docs])]).tocsr()
        clf = LogisticRegression(C=C, max_iter=3000).fit(M, ytr)
        return wv, cv, clf

    def predict(model, Xte, W=None, b=None):
        wv, cv, clf = model
        docs = [features(q, chat, syn) for q in Xte]
        M = hstack([wv.matrix([d[0] for d in docs]), cv.matrix([d[1] for d in docs])]).tocsr()
        if W is None:
            return clf.predict(M)
        return clf.classes_[np.asarray(M @ W + b).argmax(1)]

    if do_eval:
        from sklearn.model_selection import StratifiedKFold
        hits = 0
        for tr, te in StratifiedKFold(5, shuffle=True, random_state=0).split(X, y):
            m = fit([X[i] for i in tr], y[tr])
            hits += int(np.sum(predict(m, [X[i] for i in te]) == y[te]))
        print(f"5-fold CV over the phrasings: {hits / len(y):.3f}")

    model = fit(X, y)
    wv, cv, clf = model
    W = clf.coef_.T.copy()          # (features, kinds)
    keep = np.abs(W).max(1) >= PRUNE
    W[~keep] = 0
    n_keep = int(keep.sum())
    out = os.path.join(root, "data/kinds/weights.bin")
    write_weights(out, list(clf.classes_), clf.intercept_, wv, cv, W, chat, syn)
    print(f"{len(wv.index)} word + {len(cv.index)} char features, {n_keep} kept after pruning, "
          f"{len(kinds)} kinds -> {out} ({os.path.getsize(out) / 1e6:.1f} MB)")

    if do_eval:
        src = open(os.path.join(root, "tests/test_core.cpp"), encoding="utf-8").read()
        phr = set(lower_ascii(q) for q in X)
        lands = [(q, set(re.findall(r'"([^"]*)"', ks)))
                 for q, ks in re.findall(r'lands\("((?:[^"\\]|\\.)*)",\s*\{([^}]*)\}', src, re.S)]
        held = [(q, ks) for q, ks in lands if lower_ascii(q) not in phr]
        for label, Wm, b in (("unpruned", None, None), ("pruned, as shipped", W, clf.intercept_)):
            p = predict(model, [q for q, _ in lands], Wm, b)
            ok = [a in ks for a, (q, ks) in zip(p, lands)]
            okh = [a in ks for a, (q, ks) in zip(p, lands) if lower_ascii(q) not in phr]
            print(f"lands() questions ({label}): {sum(ok)}/{len(ok)} right; "
                  f"{sum(okh)}/{len(okh)} of those not themselves phrasings")
        p = predict(model, [q for q, _ in lands], W, clf.intercept_)
        for a, (q, ks) in zip(p, lands):
            if a not in ks:
                print(f"  miss: {q} -> {a} | {'/'.join(sorted(ks))}")


if __name__ == "__main__":
    main()
