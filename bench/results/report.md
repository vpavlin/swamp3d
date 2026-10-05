# Fingerprint benchmark (fp/v1)

- originals: 220, unrelated pairs (negatives): 24090, attacked copies: 660
- params: `{"samples":4096,"d2Pairs":20000,"d2Bins":64,"d2Max":3,"a3Triples":20000,"a3Bins":32,"tokTriples":30000,"angBins":12,"nrmBins":4,"minhashK":128,"bands":32,"rows":4}`

## F1 (1 - global distance)

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.953 | 18% | 0% |
| add_text | 0.975 | 87% | 62% |
| combo | 0.954 | 17% | 3% |
| decimate50 | 0.976 | 100% | 87% |
| mirror | 1.000 | 100% | 100% |
| noise | 0.976 | 87% | 68% |
| reexport | 0.983 | 100% | 100% |
| rotate | 0.985 | 100% | 100% |
| scale | 0.994 | 100% | 100% |
| stretch | 0.963 | 38% | 12% |
| subdivide | 0.980 | 100% | 93% |

thresholds: FPR 1% → 0.966, FPR 0.1% → 0.973; unrelated median 0.861, max 0.978

## F2 Jaccard

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.781 | 3% | 0% |
| add_text | 0.820 | 10% | 2% |
| combo | 0.766 | 2% | 0% |
| decimate50 | 0.828 | 8% | 3% |
| mirror | 1.000 | 98% | 95% |
| noise | 0.820 | 10% | 3% |
| reexport | 0.844 | 32% | 23% |
| rotate | 0.891 | 47% | 33% |
| scale | 0.969 | 87% | 73% |
| stretch | 0.813 | 10% | 3% |
| subdivide | 0.828 | 15% | 3% |

thresholds: FPR 1% → 0.891, FPR 0.1% → 0.922; unrelated median 0.703, max 0.961

## F2 containment (max)

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.911 | 0% | 0% |
| add_text | 0.908 | 0% | 0% |
| combo | 0.904 | 0% | 0% |
| decimate50 | 0.909 | 0% | 0% |
| mirror | 1.000 | 0% | 0% |
| noise | 0.930 | 0% | 0% |
| reexport | 0.916 | 0% | 0% |
| rotate | 0.943 | 0% | 0% |
| scale | 0.985 | 0% | 0% |
| stretch | 0.908 | 0% | 0% |
| subdivide | 0.906 | 0% | 0% |

thresholds: FPR 1% → 1.000, FPR 0.1% → 1.000; unrelated median 0.896, max 1.000

## F3 local histogram intersection

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.763 | 15% | 3% |
| add_text | 0.891 | 100% | 67% |
| combo | 0.761 | 12% | 3% |
| decimate50 | 0.892 | 88% | 67% |
| mirror | 1.000 | 100% | 100% |
| noise | 0.659 | 3% | 0% |
| reexport | 0.922 | 100% | 88% |
| rotate | 0.926 | 100% | 92% |
| scale | 0.959 | 100% | 100% |
| stretch | 0.876 | 82% | 40% |
| subdivide | 0.901 | 100% | 75% |

thresholds: FPR 1% → 0.839, FPR 0.1% → 0.880; unrelated median 0.606, max 0.976

## F3 coverage @0.7

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.874 | 5% | 2% |
| add_text | 0.948 | 65% | 10% |
| combo | 0.868 | 5% | 2% |
| decimate50 | 0.944 | 60% | 10% |
| mirror | 1.000 | 100% | 95% |
| noise | 0.845 | 12% | 0% |
| reexport | 0.960 | 90% | 23% |
| rotate | 0.965 | 92% | 23% |
| scale | 0.987 | 100% | 67% |
| stretch | 0.936 | 35% | 7% |
| subdivide | 0.948 | 67% | 8% |

thresholds: FPR 1% → 0.940, FPR 0.1% → 0.980; unrelated median 0.737, max 1.000

## F1 + F3

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.856 | 38% | 10% |
| add_text | 0.932 | 100% | 97% |
| combo | 0.857 | 35% | 8% |
| decimate50 | 0.936 | 100% | 95% |
| mirror | 1.000 | 100% | 100% |
| noise | 0.817 | 15% | 5% |
| reexport | 0.953 | 100% | 100% |
| rotate | 0.956 | 100% | 100% |
| scale | 0.975 | 100% | 100% |
| stretch | 0.917 | 98% | 75% |
| subdivide | 0.939 | 100% | 100% |

thresholds: FPR 1% → 0.874, FPR 0.1% → 0.904; unrelated median 0.725, max 0.961

## F3 IDF presence containment

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.870 | 3% | 0% |
| add_text | 0.903 | 0% | 0% |
| combo | 0.851 | 2% | 0% |
| decimate50 | 0.899 | 3% | 0% |
| mirror | 1.000 | 97% | 0% |
| noise | 0.923 | 8% | 0% |
| reexport | 0.942 | 23% | 0% |
| rotate | 0.959 | 17% | 0% |
| scale | 0.995 | 78% | 0% |
| stretch | 0.895 | 2% | 0% |
| subdivide | 0.912 | 3% | 0% |

thresholds: FPR 1% → 0.977, FPR 0.1% → 1.000; unrelated median 0.722, max 1.000

## F1 + F3 + containment

| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |
|---|---|---|---|
| add_base | 0.867 | 17% | 3% |
| add_text | 0.932 | 97% | 80% |
| combo | 0.859 | 12% | 5% |
| decimate50 | 0.936 | 97% | 80% |
| mirror | 1.000 | 100% | 100% |
| noise | 0.866 | 12% | 0% |
| reexport | 0.953 | 100% | 97% |
| rotate | 0.956 | 100% | 100% |
| scale | 0.975 | 100% | 100% |
| stretch | 0.917 | 82% | 40% |
| subdivide | 0.939 | 100% | 92% |

thresholds: FPR 1% → 0.901, FPR 0.1% → 0.920; unrelated median 0.749, max 0.961

## Most similar unrelated pairs (combined)

- 278455 vs 247069: 0.961
- 278455 vs 80353: 0.959
- 80353 vs 247069: 0.938
- 135215 vs 86849: 0.932
- 37384 vs 77513: 0.923
- 1129076 vs 74690: 0.920
- 93540 vs 57993: 0.920
- 168080 vs 511803: 0.920
- 90889 vs 278455: 0.920
- 90889 vs 80353: 0.920
