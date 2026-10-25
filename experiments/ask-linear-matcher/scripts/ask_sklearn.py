#!/usr/bin/env python3
"""scikit-learn experiments for ask's kind matcher, lexical features only.
Train on data/kinds/phrasings.tsv; test on (a) 5-fold CV over the phrasings and (b) the hand-labelled
real questions harvested from tests/test_core.cpp (lands(), ask_as(), filed[], kind_at()).
Usage: ask_sklearn.py REPO_ROOT"""
import sys, os, re, collections, warnings
import numpy as np
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.svm import LinearSVC
from sklearn.linear_model import LogisticRegression, SGDClassifier, RidgeClassifier
from sklearn.naive_bayes import ComplementNB
from sklearn.neighbors import KNeighborsClassifier
from sklearn.model_selection import StratifiedKFold, cross_val_predict
from sklearn.pipeline import make_pipeline, make_union
warnings.filterwarnings("ignore")
root = sys.argv[1]
rng = np.random.RandomState(0)

# ---------------------------------------------------------------- data
X, y = [], []
for line in open(os.path.join(root, "data/kinds/phrasings.tsv"), encoding="utf-8"):
    line = line.rstrip("\n")
    if "\t" in line:
        k, p = line.split("\t", 1); X.append(p); y.append(k)
X = np.array(X); y = np.array(y)
kinds = sorted(set(y)); counts = collections.Counter(y)
phr_set = set(s.lower() for s in X)

src = open(os.path.join(root, "tests/test_core.cpp"), encoding="utf-8").read()
held = collections.OrderedDict()   # question -> set of accepted kinds
def add(q, ks):
    if q.lower() in phr_set: return   # exact phrasing: not held out
    held.setdefault(q, set()).update(ks)
for q, ks in re.findall(r'lands\("((?:[^"\\]|\\.)*)",\s*\{([^}]*)\}', src, re.S):
    add(q, re.findall(r'"([^"]*)"', ks))
for k, q in re.findall(r'ask_as\("([a-z][^"]*)",\s*"([^"]*)"\)', src):
    add(q, [k])
for q, k in re.findall(r'filed\["([^"]*)"\] == "([^"]*)"', src):
    add(q, [k])
for q, k in re.findall(r'kind_at\("([^"]*)"\) == "([^"]*)"', src):
    add(q, [k])
held = {q: ks for q, ks in held.items() if all(k in counts for k in ks)}
HQ = np.array(list(held.keys())); HK = [held[q] for q in HQ]
ood = ["did the sun crash yesterday", "how do i cook a steak", "did my mum use the laptop", "tell me a joke",
       "what is the meaning of life", "whats the capital of france", "write me a poem about autumn",
       "how tall is the eiffel tower", "translate hello to spanish", "who won the match last night",
       "what should i have for dinner", "is it going to rain tomorrow"]
print(f"train: {len(X)} phrasings, {len(kinds)} kinds")
print(f"held-out real questions from tests: {len(HQ)} (exact phrasings excluded); "
      f"{sum(len(k) > 1 for k in HK)} accept more than one kind; {len(set(k for ks in HK for k in ks))} kinds covered")

# ---------------------------------------------------------------- normalisation like unllm's speller/synonyms
def table(path, sep):
    t = {}
    for line in open(os.path.join(root, path), encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#") or sep not in line: continue
        a, b = line.split(sep, 1)
        if sep == ":":
            for w in b.split(): t[w] = a.strip()
        else:
            t[a.strip()] = b.strip()
    return t
chat = table("vendor/unllm/data/chatspeak.txt", "->"); syn = table("vendor/unllm/data/synonyms.txt", ":")
def norm(s):
    out = []
    for w in re.findall(r"[a-z0-9%]+", s.lower()):
        w = chat.get(w, w)
        for piece in w.split(): out.append(syn.get(piece, piece))
    return " ".join(out)
Xl = np.array([s.lower() for s in X]); Xn = np.array([norm(s) for s in X])
HQl = np.array([s.lower() for s in HQ]); HQn = np.array([norm(s) for s in HQ])

# ---------------------------------------------------------------- helpers
def feats(word=(1, 2), char=(3, 5)):
    parts = []
    if word: parts.append(TfidfVectorizer(ngram_range=word, sublinear_tf=True))
    if char: parts.append(TfidfVectorizer(analyzer="char_wb", ngram_range=char, sublinear_tf=True, min_df=2))
    return make_union(*parts) if len(parts) > 1 else parts[0]
def axes(k):
    p = k.split(".")
    w = "window" if "window" in p else ("now" if "now" in p else "either")
    s = "prog" if p[-1] in ("program", "process", "unit", "system") else "none"
    f = ".".join(q for q in p[1:] if q not in ("now", "window", "program", "process", "unit", "system"))
    return p[0], f, w, s
def nowin(k): return k.replace(".window", "").replace(".now", "").replace(".either", "")
cv = StratifiedKFold(5, shuffle=True, random_state=0)
def cv_acc(pipe, Xs):
    p = cross_val_predict(pipe, Xs, y, cv=cv); return np.mean(p == y), p
def held_acc(pipe, Xs, Hs, k=1):
    pipe.fit(Xs, y)
    if k == 1:
        p = pipe.predict(Hs); return np.mean([a in ks for a, ks in zip(p, HK)]), p
    sc = pipe.decision_function(Hs); cls = pipe.classes_
    top = cls[np.argsort(-sc, axis=1)[:, :k]]
    return np.mean([any(a in ks for a in row) for row, ks in zip(top, HK)]), top

# ---------------------------------------------------------------- 1. decision rules, both test sets
print("\n== 1. decision rule (features: word 1-2g + char 3-5g, lowercased) ==")
print(f"{'model':38s} {'CV':>6s} {'held-out':>9s}")
rows = [
    ("1-NN cosine (today's rule)", lambda: make_pipeline(feats(), KNeighborsClassifier(1, metric="cosine"))),
    ("5-NN distance-weighted", lambda: make_pipeline(feats(), KNeighborsClassifier(5, metric="cosine", weights="distance"))),
    ("ComplementNB", lambda: make_pipeline(feats(), ComplementNB(alpha=0.3))),
    ("RidgeClassifier", lambda: make_pipeline(feats(), RidgeClassifier(alpha=1.0))),
    ("SGD modified_huber", lambda: make_pipeline(feats(), SGDClassifier(loss="modified_huber", alpha=1e-5, max_iter=50, random_state=0))),
    ("LinearSVC C=0.3", lambda: make_pipeline(feats(), LinearSVC(C=0.3))),
    ("LinearSVC C=1", lambda: make_pipeline(feats(), LinearSVC(C=1.0))),
    ("LinearSVC C=3", lambda: make_pipeline(feats(), LinearSVC(C=3.0))),
    ("LogisticRegression C=20", lambda: make_pipeline(feats(), LogisticRegression(C=20, max_iter=3000))),
]
best = None
for name, mk in rows:
    a, _ = cv_acc(mk(), Xl); h, _ = held_acc(mk(), Xl, HQl)
    print(f"{name:38s} {a:6.3f} {h:9.3f}")
    if name.startswith("LinearSVC C=1"): best = mk

# ---------------------------------------------------------------- 2. features
print("\n== 2. features (LinearSVC C=1) ==")
print(f"{'features':38s} {'CV':>6s} {'held-out':>9s}")
for name, w, c, tr, hq in [
    ("word 1g only", (1, 1), None, Xl, HQl),
    ("word 1-2g only", (1, 2), None, Xl, HQl),
    ("word 1-3g only", (1, 3), None, Xl, HQl),
    ("char 3-5g only", None, (3, 5), Xl, HQl),
    ("char 2-6g only", None, (2, 6), Xl, HQl),
    ("word 1-2g + char 3-5g", (1, 2), (3, 5), Xl, HQl),
    ("same + chatspeak/synonym normalisation", (1, 2), (3, 5), Xn, HQn),
    ("word 1-3g + char 2-6g, normalised", (1, 3), (2, 6), Xn, HQn),
]:
    mk = lambda w=w, c=c: make_pipeline(feats(w, c), LinearSVC(C=1.0))
    a, _ = cv_acc(mk(), tr); h, _ = held_acc(mk(), tr, hq)
    print(f"{name:38s} {a:6.3f} {h:9.3f}")

# ---------------------------------------------------------------- 3. top-k, window-collapsed, per-axis on held-out
print("\n== 3. what the errors are (LinearSVC C=1, word+char, lowercased) ==")
mk = best
a1, pcv = cv_acc(mk(), Xl)
for k in (1, 2, 3):
    h, _ = held_acc(mk(), Xl, HQl, k); print(f"held-out top-{k}: {h:.3f}")
pipe = mk().fit(Xl, y); ph = pipe.predict(HQl)
print(f"held-out, window axis ignored (unllm's time reader decides it): "
      f"{np.mean([nowin(a) in {nowin(k) for k in ks} for a, ks in zip(ph, HK)]):.3f}")
print(f"CV, window axis ignored: {np.mean([nowin(a) == nowin(b) for a, b in zip(pcv, y)]):.3f}")
ya = np.array([axes(k) for k in y]); pa = np.array([axes(k) for k in pcv])
for i, n in enumerate(["topic", "form", "window", "program-slot"]):
    hv = np.mean([axes(a)[i] in {axes(k)[i] for k in ks} for a, ks in zip(ph, HK)])
    print(f"  axis {n:13s}: CV {np.mean(pa[:, i] == ya[:, i]):.3f}   held-out {hv:.3f}")
print("held-out misses (question -> predicted | accepted):")
for q, a, ks in zip(HQ, ph, HK):
    if a not in ks: print(f"  {q[:70]:70s} -> {a} | {'/'.join(sorted(ks))}")

# ---------------------------------------------------------------- 4. constrained axis heads
print("\n== 4. axis heads, scored over valid kinds only ==")
valid = {k: axes(k) for k in kinds}
def axis_scores(train_idx, test_X):
    f = feats().fit(Xl[train_idx]); Ft = f.transform(Xl[train_idx]); Fe = f.transform(test_X)
    total = np.zeros((Fe.shape[0], len(kinds)))
    for i in range(4):
        lab = ya[train_idx, i]; clf = LinearSVC(C=1.0).fit(Ft, lab)
        sc = clf.decision_function(Fe)
        if sc.ndim == 1: sc = np.stack([-sc, sc], 1)
        col = {c: j for j, c in enumerate(clf.classes_)}
        for kj, k in enumerate(kinds):
            total[:, kj] += sc[:, col[valid[k][i]]] if valid[k][i] in col else -1.0
    return np.array(kinds)[np.argmax(total, 1)]
pc = np.empty(len(y), dtype=object)
for tr, te in cv.split(Xl, y): pc[te] = axis_scores(tr, Xl[te])
print(f"CV: {np.mean(pc == y):.3f}")
ph4 = axis_scores(np.arange(len(y)), HQl)
print(f"held-out: {np.mean([a in ks for a, ks in zip(ph4, HK)]):.3f}")

# ---------------------------------------------------------------- 5. confidence and refusal
print("\n== 5. confidence: can one threshold replace the hand-set cosine gates? (LogisticRegression) ==")
lr = make_pipeline(feats(), LogisticRegression(C=20, max_iter=3000))
proba = cross_val_predict(lr, Xl, y, cv=cv, method="predict_proba")
cls = np.array(sorted(set(y))); pred = cls[proba.argmax(1)]; conf = proba.max(1)
print("CV phrasings: coverage and accuracy of the answered part at each confidence floor")
for t in (0.0, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
    m = conf >= t; print(f"  conf >= {t:.1f}: answer {m.mean():5.1%} of questions, {np.mean(pred[m] == y[m]):.3f} right")
lr.fit(Xl, y); hp = lr.predict_proba(HQl); hc = hp.max(1); hpred = lr.classes_[hp.argmax(1)]
right = np.array([a in ks for a, ks in zip(hpred, HK)])
print(f"held-out: mean confidence when right {hc[right].mean():.2f}, when wrong {hc[~right].mean():.2f}")
oc = lr.predict_proba([s.lower() for s in ood]).max(1)
print(f"out-of-domain questions: max confidence {oc.max():.2f}, mean {oc.mean():.2f}  (in-domain held-out mean {hc.mean():.2f})")
for q, c, k in zip(ood, oc, lr.classes_[lr.predict_proba([s.lower() for s in ood]).argmax(1)]):
    print(f"  {c:.2f}  {q:40s} -> {k}")

# ---------------------------------------------------------------- 6. learning curve: phrasings per kind
print("\n== 6. how many phrasings per kind are needed (LinearSVC, held-out real questions) ==")
by_kind = collections.defaultdict(list)
for i, k in enumerate(y): by_kind[k].append(i)
for cap in (5, 10, 20, 40, 80, None):
    idx = np.concatenate([rng.permutation(v)[:cap] if cap else v for v in by_kind.values()])
    p = make_pipeline(feats(), LinearSVC(C=1.0)).fit(Xl[idx], y[idx]).predict(HQl)
    print(f"  <= {str(cap or 'all'):>3s} per kind ({len(idx):4d} phrasings): held-out {np.mean([a in ks for a, ks in zip(p, HK)]):.3f}")

# ---------------------------------------------------------------- 7. catalogue: which kinds are indistinguishable
print("\n== 7. kinds the classifier cannot tell apart (CV, LinearSVC) ==")
conf2 = collections.Counter()
for a, b in zip(y, pcv):
    if a != b: conf2[tuple(sorted((a, b)))] += 1
for (a, b), n in conf2.most_common(12):
    print(f"  {n:3d} both ways  {a}  <->  {b}   ({counts[a]}, {counts[b]} phrasings)")
