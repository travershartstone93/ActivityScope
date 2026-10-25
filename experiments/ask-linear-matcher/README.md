# Ask: a trained linear matcher instead of nearest-phrasing

Written 2026-10-25 in a cloud session for the local Claude Code instance. Nothing here is
applied to the tree: the patches, trainer and scripts are a proposal with its evidence. Read
this file, then decide with the author which steps to apply.

## The problem

`ask` answers correctly about half the time in use. Its matcher (`src/core/kinds.cpp`,
`Kinds::match`) embeds the question with bge-small and takes the kind of the **single nearest
phrasing** among the 5,913 in `data/kinds/phrasings.tsv` (138 kinds, ~43 phrasings each).
Everything downstream in `askcore.cpp` keys off that one cosine score with hand-set gates
(0.80 domain/window/form at ~850, 0.70 for records at ~1196, 0.85 at ~1435, a 0.02 runner band
at ~795).

The author asked whether to replace bge-small with mlpack/ensmallen or a Qwen3-Reranker-0.6B.
The answer this work supports: neither. The weak part is the *decision rule* (1-NN), not the
embedder, and the catalogue itself caps accuracy.

## What was measured (all on the phrasings and the labelled questions in tests/test_core.cpp)

`experiment-report.txt` is the full output of `scripts/ask_sklearn.py`. Lexical features only
(word unigrams/n-grams + character 3-5-grams); the bge-small model could not be downloaded in
the cloud container, so no hybrid or direct bge-small comparison exists yet. Key numbers:

| decision rule, same features            | 5-fold CV on phrasings | 173 held-out test questions |
|-----------------------------------------|-----:|-----:|
| 1-NN cosine (today's rule)              | 0.511 | 0.699 |
| LinearSVC                               | 0.764 | 0.809 |
| LogisticRegression C=20                 | 0.772 | 0.832 |

- Word bigrams/trigrams **hurt** (0.751 -> 0.705 -> 0.672 CV): too few examples per kind.
  Unigrams + char-grams is the right feature set.
- Per axis (held-out): topic 0.91, **form 0.86**, window 0.90, program-slot 0.99. Form
  (value / cause / which / often) is the weak axis.
- Top-2 accuracy 0.925, top-3 0.942: the right kind is nearly always a runner-up.
- Window axis ignored (unllm's time reader decides it anyway): 0.838.
- Learning curve: accuracy rises steeply to 40 phrasings per kind and is flat after. **19 kinds
  have fewer than 15 phrasings** and score 0.38; adding phrasings to kinds already over 40 buys
  nothing.
- Pairs the classifier cannot separate (CV): `disk.value.now <-> disk.which.now`,
  `startup.often.reboot <-> startup.often.window`, `sharing.which.now <-> sharing.yesno.either`,
  `diagnose.list.window <-> slow.list.window`, `slow.load.value.now <-> slow.value.now`,
  `network.value.now <-> network.which.now`. These are catalogue boundaries, not model errors.
- LogisticRegression confidence separates right (mean 0.74) from wrong (0.42) answers; the
  catalogue's `none` kind catches most out-of-domain questions, but lexical features cannot
  know "the sun" is not a program ("did the sun crash yesterday" -> crashes.list.window at
  0.87). That check belongs to `Names`/the embedder, not the matcher.

**Caveat.** The 173 "held-out" questions come from `lands()`, `ask_as()`, `filed[]` and
`kind_at()` in test_core.cpp. The author wrote phrasings until bge-small passed the `lands()`
set, so on that set bge-small is ~100% by construction and these numbers are optimistic for
any model. The gaps *between* rows are what to trust. The comparison that settles it is on
questions neither matcher has seen: a log of real use.

## What was built and tested

`linear-matcher.patch` (against `ed12626`) and `kindtrain.py`:

- `tools/kindtrain.py` trains multinomial logistic regression on the phrasings (chatspeak and
  synonym tables from `vendor/unllm/data` applied first, word unigrams + char 3-5-grams,
  tf-idf 1+ln tf, smoothed idf, unit length per block) and writes `data/kinds/weights.bin`
  (binary: kind ids and biases, features with idf, float32 weight matrix, the two tables).
  5.3 MB; regenerate it, do not commit it. `--eval` prints CV and the `lands()` results.
- `src/core/kinds.cpp`: `Kinds::load` loads `weights.bin` when present; `Kinds::match` then
  scores every kind and returns **probabilities** (`score`, `runners`), with each top-8
  runner's closest phrasing by word overlap in `runner_phrasings`. Without the file the old
  nearest-phrasing path is unchanged. The embedding model becomes optional for matching
  (`Kinds::model()` still exposes it for `Names`). Tokenisation is defined identically in
  Python and C++ (ASCII lower case, runs of `[a-z0-9%]`).
- `tests/test_core.cpp`: `test_kinds` runs its matcher checks when the model **or**
  `weights.bin` exists.

Results in the cloud container (no bge-small):

- `test_kinds`: **81 of 86 `lands()` questions right**; the C++ and Python models produce the
  identical 5 misses (`how many processes are running right now`, `wat was the laptop up to the
  other night before i shut the lid`, `was anything cryptomining on this machine last night`,
  `bios settings changed`, `did someone mess with the uefi settings`).
- Load 40 ms, **122 us per question** (`scripts/kindtime.cpp`), vs ~2.7 ms before pre-tokenising
  the phrasings.
- Full suite: 573 checks, 16 failed = the 5 above + 11 pre-existing failures in `test_names`,
  which loads the embedding model without the skip gate the other tests have.

On the `lands()` set the linear matcher is therefore 5 questions behind bge-small, on a set
tuned for bge-small. It is not yet shown to be better in the field; it is shown to be a working,
fast, dependency-free matcher with calibrated confidence.

## Two build problems found on the way (`build-fixes.patch`)

1. `vendor/unllm/src/meaning.cpp:244`: `(void)question;` in the `#else` (no ONNX Runtime)
   branch names a parameter that does not exist (`text`, `query`). The tree does not compile
   on any machine without ONNX Runtime. Fix: `(void)text; (void)query;`.
2. `CMakeLists.txt:21` links `SQLite3::SQLite3`; CMake < 4 (e.g. 3.28) only defines
   `SQLite::SQLite3`. Either name it that or link `sqlite3` as `vendor/unllm` does.

Also noticed, not patched: `vendor/unllm/CMakeLists.txt:92` tests `${CMAKE_SOURCE_DIR}/tests/...`
instead of `CMAKE_CURRENT_SOURCE_DIR`, so unllm's dialogue/world/calc/extract exams and the
ingest test are not registered when built as a subdirectory; `tests/test_hindsight.py` fails
two tests when run as root (a `chmod 0` file is readable); `CMakeLists.txt` defines
`FIXTURES=tests/fixtures`, which does not exist (harmless).

## The plan toward "right, or asks, close to 100% of the time"

No matcher gets free text right ~100% first time. The reachable target is: right, or one short
clarifying question, never silently wrong, and each clarification teaches it. In order:

1. **Instrument.** Log every question with kind, confidence, window, program and branch to an
   `ask_log` table in history.db; `ask --wrong` flags the last one. A week of this is the only
   test set that counts. Everything below is tuned against it.
2. **Trained matcher** (this patch), then the **hybrid**: append bge-small's 384-d vector as a
   third feature block when the model is present (`kindtrain.py` needs the vectors; the C++
   side gets them from `Meaning::embed`). Compare both against today's matcher on the step-1 log.
3. **Window from the reader.** Train on kinds with `now`/`window` stripped; `askcore.cpp:~809`
   already picks the variant from `reading.window`. Removes the now/window confusions.
4. **Clarify instead of guess.** Below ~0.5 confidence with a close runner-up, return two
   readings in plain words (`Ask::kind_words`) and let the user pick; the pick re-asks with
   `--branch`. The answer builder already keeps "how the question was taken" as a part: show
   it on every answer so a wrong reading is visible at once.
5. **Learn from the pick.** Append `kind<TAB>question` to a local phrasings file under
   `~/.local/share/activityscope/kinds/`, merge it in `Kinds::load`, re-fit the linear weights
   in C++ (a few hundred SGD steps; no library). The same miss never repeats for that user.
6. **Catalogue.** Fill the 19 thin kinds to ~40 phrasings; merge or redefine the inseparable
   pairs above, or give each merged kind a composite answer (value *and* culprit, often *and*
   when) so the matcher need not pick between readings nothing distinguishes.
7. Only if a residue remains: route low-confidence questions to qwen3:4b via Ollama when it is
   running, to pick among the top candidate kinds.

mlpack/ensmallen are not needed for any step; Qwen3-Reranker-0.6B (1.5 GB, seconds on CPU)
does not fit the product and attacks the wrong problem.

## How to apply and A/B on the author's machine (bge-small installed)

    git apply experiments/ask-linear-matcher/build-fixes.patch      # if the tree does not build here
    git apply experiments/ask-linear-matcher/linear-matcher.patch
    cp experiments/ask-linear-matcher/kindtrain.py tools/
    python3 tools/kindtrain.py . --eval          # writes data/kinds/weights.bin (needs scikit-learn, scipy)
    cmake --build build && ./build/ascore_tests  # linear matcher (weights.bin takes precedence)
    rm data/kinds/weights.bin && ./build/ascore_tests   # bge-small nearest-phrasing, as today

With the model present, `test_names`, `records_n50`, `live_t95`, `records_t95` and the
event/catalogue/window checks also run; they were skipped in the cloud. Expect `askcore.cpp`'s
cosine thresholds (0.70-0.85) to need re-tuning for probabilities, or better, replacing with
one confidence threshold (step 4).

## Files

- `README.md` - this file
- `linear-matcher.patch` - kinds.cpp, kinds.h, test_core.cpp
- `build-fixes.patch` - meaning.cpp, CMakeLists.txt
- `kindtrain.py` - the trainer (goes in tools/)
- `experiment-report.txt` - output of scripts/ask_sklearn.py
- `scripts/ask_sklearn.py` - the full experiment battery (decision rules, features, top-k,
  axis heads, confidence, learning curve, confusions); takes the repo root
- `scripts/sparse_cv.py`, `scripts/variants.py` - the first two smaller runs
- `scripts/kindtime.cpp` - load and per-question timing of `Kinds::match`
