# Stage 2: geometric coverage

- positives 720, hard negatives 300 (top-5 shortlist look-alikes per original)
- time per pair (both directions): median 2444 ms

## coverage (max of both directions)

hard negatives: median 0.724, p90 0.954, max 1.000; FPR 1% above 1.000, FPR 5% above 0.981

| attack | median | p10 | detected @ FPR 1% | @ FPR 5% |
|---|---|---|---|---|
| add_base | 0.965 | 0.749 | 0% | 38% |
| add_text | 0.962 | 0.676 | 0% | 37% |
| combo | 0.943 | 0.679 | 0% | 33% |
| cut60 | 0.868 | 0.631 | 0% | 23% |
| decimate50 | 0.965 | 0.664 | 0% | 43% |
| mirror | 0.931 | 0.601 | 0% | 25% |
| noise | 0.891 | 0.547 | 0% | 18% |
| reexport | 0.941 | 0.655 | 0% | 35% |
| rotate | 0.967 | 0.692 | 0% | 42% |
| scale | 0.968 | 0.666 | 0% | 38% |
| stretch | 0.888 | 0.599 | 0% | 15% |
| subdivide | 0.949 | 0.730 | 0% | 42% |

worst hard negatives: 100471~856327 1.00, 101647~511803 1.00, 168080~176193 1.00, 58874~161091 1.00, 168080~78324 1.00, 135363~54161 1.00

## distinctive coverage (max)

hard negatives: median 0.710, p90 0.951, max 1.000; FPR 1% above 1.000, FPR 5% above 0.981

| attack | median | p10 | detected @ FPR 1% | @ FPR 5% |
|---|---|---|---|---|
| add_base | 0.963 | 0.727 | 0% | 38% |
| add_text | 0.949 | 0.695 | 0% | 35% |
| combo | 0.931 | 0.680 | 0% | 32% |
| cut60 | 0.856 | 0.633 | 0% | 20% |
| decimate50 | 0.962 | 0.640 | 0% | 40% |
| mirror | 0.918 | 0.513 | 0% | 25% |
| noise | 0.887 | 0.541 | 0% | 17% |
| reexport | 0.933 | 0.660 | 0% | 33% |
| rotate | 0.959 | 0.684 | 0% | 38% |
| scale | 0.961 | 0.686 | 0% | 37% |
| stretch | 0.873 | 0.555 | 0% | 18% |
| subdivide | 0.948 | 0.687 | 0% | 42% |

worst hard negatives: 100471~856327 1.00, 101647~511803 1.00, 168080~176193 1.00, 58874~161091 1.00, 168080~78324 1.00, 376260~741525 1.00

