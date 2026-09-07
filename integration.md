# OpenGL 4.6 integration

The repository currently provides the validated C++ control core and GLSL 4.60 pass contracts. The
window/context and real OpenGL object adapter remain the next implementation boundary.

## Required inputs

| Binding | Resource | Required property |
| --- | --- | --- |
| texture 0 | current color | reduced or native current raster |
| texture 1 | current depth | fixed documented depth convention |
| texture 2 | motion vectors | current UV to previous UV delta |
| texture 3 | reactive mask | rejects unstable/translucent history |
| texture 4 | history color | native-resolution previous proven result |
| texture 5 | history depth | native-resolution previous depth |
| texture 6 | current identity | unsigned EIN slot and generation |
| texture 7 | history identity | unsigned EIN slot and generation |
| UBO 8 | relay constants | extents, mode, generation, weight and depth scale |

History and output images must be different textures. Never sample from and write to the same image
in a relay dispatch.

## Frame execution

```cpp
// 1. Classify temporal continuity and build native-refine work.
glUseProgram(fullPathRelayProgram);
glDispatchCompute(divUp(width, 8), divUp(height, 8), 1);
glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                GL_TEXTURE_FETCH_BARRIER_BIT);

// 2. Shade the refine mask at native quality into a separate attachment.
renderNativeRefineTiles();
glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT |
                GL_TEXTURE_FETCH_BARRIER_BIT);

// 3. Resolve only proven relay or valid native work.
clearProofCounters();
glUseProgram(fullPathResolveProgram);
glDispatchCompute(divUp(width, 8), divUp(height, 8), 1);
glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                GL_SHADER_STORAGE_BARRIER_BIT);

// 4. Fence, read C11 proof data, then conditionally swap C10 history.
```

The proof-counter read must wait for its fence. A non-zero `unresolvedPixels` value prohibits the
history swap and requests a FULL frame.

For the compute-first sparse path, insert the three classification stages between relay
classification and native shading. Their bindings, queue layouts, indirect dispatch sequence, and
overflow rules are specified in `classification-units.md`.

## Optional LAN resources

The LAN resource coordinator may sit between classification queue creation and native shading.
Lease only self-contained 128×128 massive units; retain the FULL seed and history commit on the
presenting machine. Discovery uses TTL-one UDP multicast, while work payloads and results must use
an application-owned authenticated and encrypted carrier.

Run `LanAdministrator::expire()` every scheduler tick. Every returned fallback request goes back to
the corresponding local classification or native-refine queue before resolve. Never allow a remote
timeout to leave a tile unresolved. The complete contract is in `lan-administrators.md`.

## Integration with a renderer

Keep the repository as a module beneath the application layer:

```text
renderer scene + materials + motion
             ↓
EIN Full-Path Relay
  classify → native refine → resolve → prove
             ↓
native output / proven history
```

An OpenGLFramework-style host should place the future adapter under `graphics/opengl` and the relay
passes under `graphics/passes`. The public feature API remains free of raw `GLuint` values.
