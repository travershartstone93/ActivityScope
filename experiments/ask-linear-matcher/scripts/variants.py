import sys, numpy as np, collections
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.svm import LinearSVC
from sklearn.neighbors import KNeighborsClassifier
from sklearn.model_selection import StratifiedKFold, cross_val_predict
from sklearn.pipeline import make_pipeline, make_union
X,y=[],[]
for l in open(sys.argv[1],encoding="utf-8"):
    l=l.rstrip("\n")
    if "\t" in l: k,p=l.split("\t",1); X.append(p.lower()); y.append(k)
X=np.array(X); y=np.array(y)
def axes(k):
    p=k.split("."); w="window" if "window" in p else ("now" if "now" in p else "either")
    s="prog" if p[-1] in ("program","process","unit","system") else "none"
    f=".".join(q for q in p[1:] if q not in ("now","window","program","process","unit","system"))
    return p[0],f,w,s
feat=lambda: make_union(TfidfVectorizer(ngram_range=(1,2),sublinear_tf=True),TfidfVectorizer(analyzer="char_wb",ngram_range=(3,5),sublinear_tf=True,min_df=2))
cv=StratifiedKFold(5,shuffle=True,random_state=0)
acc=lambda a,b: float(np.mean(np.array(a)==np.array(b)))
# 1-NN with lexical features: the current decision rule, different features
p1=cross_val_predict(make_pipeline(feat(),KNeighborsClassifier(1,metric="cosine")),X,y,cv=cv)
print(f"1-NN (nearest phrasing), lexical features : {acc(p1,y):.3f}")
p5=cross_val_predict(make_pipeline(feat(),KNeighborsClassifier(5,metric="cosine",weights="distance")),X,y,cv=cv)
print(f"5-NN weighted, lexical features           : {acc(p5,y):.3f}")
# per-axis heads, composed back into a kind id
ya=np.array([axes(k) for k in y]); comp=[]
heads=[cross_val_predict(make_pipeline(feat(),LinearSVC()),X,ya[:,i],cv=cv) for i in range(4)]
for i,n in enumerate(["topic","form","window","prog"]): print(f"  head {n:7s}: {acc(heads[i],ya[:,i]):.3f}")
valid={axes(k):k for k in set(y)}
comp=[valid.get((a,b,c,d),"<invalid>") for a,b,c,d in zip(*heads)]
print(f"composed from axis heads                  : {acc(comp,y):.3f}  (invalid combos: {sum(c=='<invalid>' for c in comp)})")
# coverage: how much is explained by thin kinds
cnt=collections.Counter(y); thin=np.array([cnt[k]<15 for k in y])
pf=cross_val_predict(make_pipeline(feat(),LinearSVC()),X,y,cv=cv)
print(f"flat SVM: kinds with >=15 phrasings {acc(pf[~thin],y[~thin]):.3f}, thin kinds (<15) {acc(pf[thin],y[thin]):.3f} ({thin.sum()} phrasings in {sum(1 for k,c in cnt.items() if c<15)} kinds)")
