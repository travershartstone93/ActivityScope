#!/usr/bin/env python3
"""Cross-validated kind classification on data/kinds/phrasings.tsv with lexical features only
(no neural embedder). Reports accuracy on the full kind id and on each axis of it."""
import sys, collections
import numpy as np
from scipy.sparse import hstack
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.svm import LinearSVC
from sklearn.model_selection import StratifiedKFold, cross_val_predict
from sklearn.pipeline import make_pipeline, make_union

path = sys.argv[1]
X, y = [], []
for line in open(path, encoding="utf-8"):
    line = line.rstrip("\n")
    if not line or "\t" not in line:
        continue
    k, p = line.split("\t", 1)
    X.append(p.lower()); y.append(k)
X = np.array(X); y = np.array(y)
print(f"{len(X)} phrasings, {len(set(y))} kinds, {len(set(X))} distinct texts")

def axes(kid):
    parts = kid.split(".")
    topic = parts[0]
    window = "window" if "window" in parts else ("now" if "now" in parts else "either")
    prog = "program" if parts[-1] in ("program", "process", "unit", "system") else "none"
    form = ".".join(p for p in parts[1:] if p not in ("now", "window", "program", "process", "unit", "system"))
    return topic, form, window, prog

counts = collections.Counter(y)
feat = make_union(
    TfidfVectorizer(ngram_range=(1, 2), sublinear_tf=True, min_df=1),
    TfidfVectorizer(analyzer="char_wb", ngram_range=(3, 5), sublinear_tf=True, min_df=2),
)
clf = make_pipeline(feat, LinearSVC(C=1.0))
min_count = min(counts.values())
folds = max(2, min(5, min_count))
cv = StratifiedKFold(n_splits=folds, shuffle=True, random_state=0)
pred = cross_val_predict(clf, X, y, cv=cv)

def acc(a, b): return float(np.mean(np.array(a) == np.array(b)))
print(f"\n{folds}-fold CV, tf-idf word 1-2g + char 3-5g, linear SVM")
print(f"  exact kind         : {acc(pred, y):.3f}")
ya = np.array([axes(k) for k in y]); pa = np.array([axes(k) for k in pred])
for i, name in enumerate(["topic", "form", "window", "program-slot"]):
    print(f"  {name:19s}: {acc(pa[:, i], ya[:, i]):.3f}")

# Where it goes wrong: same topic, different form/window?
wrong = pred != y
same_topic = (pa[:, 0] == ya[:, 0]) & wrong
print(f"\n{wrong.sum()} errors; {same_topic.sum()} of them keep the topic right (form/window/program confusion)")
conf = collections.Counter((a, b) for a, b, w in zip(y, pred, wrong) if w)
print("\nmost confused pairs (true -> predicted):")
for (a, b), n in conf.most_common(15):
    print(f"  {n:3d}  {a} -> {b}")
print("\nkinds with lowest recall:")
per = collections.defaultdict(lambda: [0, 0])
for a, b in zip(y, pred):
    per[a][1] += 1; per[a][0] += a == b
for k, (c, n) in sorted(per.items(), key=lambda kv: kv[1][0] / kv[1][1])[:12]:
    print(f"  {c:3d}/{n:<3d} {k}")
