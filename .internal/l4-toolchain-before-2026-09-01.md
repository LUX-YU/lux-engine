# L4 Toolchain Before Evidence

Baseline: `bb2b12b3f425db16eca6a96828a101fd3ae9bad5`

Date: 2026-09-01

## Qualification baseline

```text
Default Developer / packed OFF: 150/150 CTest
Full Render / packed ON:        163/163 CTest
TOOLCHAIN profile:              142/142 CTest
TOOLCHAIN second build:         ninja: no work to do
```

## Material fixture golden

```text
605ebb80-8566-4f2a-80b6-a153fead1eec.luxasset
  bytes: 62358
  SHA-256: DAA45C90CBF984ACCF0E6A7DE2A1D8E9060BD821CAEC8923E6423004B373037A

ccf44e8f-2220-459f-81a8-566d5c6d4b26.luxasset
  bytes: 66269
  SHA-256: D49B59DE705992068D5CDE6A26293744098082FFDF5E6174B021A790E19B2231

ce5f070e-f086-49da-b516-bf6febd75a0f.luxasset
  bytes: 64609
  SHA-256: DD12ACB0AE8DF4DD309F1E9037AE7808A8A3F36A80A0EF2D58A195E194F52197

static_model.luxpak
  SHA-256: 7728D9C0E6A5BBD6E7FCC962E5B7F628CF8F40DA84A1580AF766E4F0DD08703B
```

## Preserved user changes

The pre-existing `.gitignore` change and formatting-only change in
`engine/domain/world/partition/include/lux/engine/world/WorldPartition.hpp` are outside this wave and remain
uncommitted.
