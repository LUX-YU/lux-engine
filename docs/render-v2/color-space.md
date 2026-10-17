# Working color and texture sampling

Scene working RGB is linear-light BT.2020 primaries, D65 white. Alpha is coverage, never gamma transformed.
This defines source/sampling semantics, not HDR display support. Numerical parameter values must not
be silently reinterpreted when applying a texture color transform.

- SRGB: BT.709/D65 encoded RGB. Native SRGB texture sampling applies EOTF exactly once; sampled shader
  then converts linear BT.709 to linear BT.2020. It must NOT apply another EOTF.
- Historical LINEAR: explicit compatibility input is linear BT.709/D65, followed by the same primaries matrix.
  Existing metadata/cooker provide no evidence of BT.2020 input; no silent wide-gamut reinterpretation.
- DATA: UNORM/float components are numerical data, no EOTF or primaries conversion. Reject SRGB-format DATA.
- UNKNOWN: reject; no unproven implicit conversion.
- Straight alpha remains straight. Linear premultiplied input stays premultiplied under the linear matrix.
  Encoded-space premultiplied SRGB cannot be decoded as ordinary SRGB; reject that ambiguous unsupported
  combination rather than double-transforming or pretending hardware decoding commutes with multiplication.

Reference BT.709→BT.2020 matrix (row-major):
```
0.6274038959  0.3292830384  0.0433130657
0.0690972894  0.9195403951  0.0113623156
0.0163914389  0.0880133079  0.8955952532
```
CPU oracle uses double sRGB EOTF: c/12.92 for c≤0.04045, otherwise ((c+0.055)/1.055)^2.4.
GPU RGBA8 offscreen comparison tolerates 2/255 for conversion/quantization; DATA exact integer samples
must match. Test colored primaries, midtones, alpha, mip selection and region replacement, not just white.
The matrix is fixed in the sampled ABI; no per-frame color lookup or new ColorManager.

These rules are exercised by the Native TextureSampling fixture and its reference shader. They do not
automatically establish color conversion for every material program. A material's cooked Shader contract
must declare and implement its actual source-to-working transform. Display output, tonemapping and HDR
transfer remain separate from texture decoding.
