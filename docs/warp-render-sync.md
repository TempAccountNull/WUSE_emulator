# WARP render synchronization

NtGdiDdDDIRender returned success without processing its command buffer. The
game submitted SYNC(24); WARP expected SYAK(8), rejected the unchanged header,
recorded E_OUTOFMEMORY at d3d10warp.dll+0x99DE9, and reported device removal
through d3d11.dll+0x122FC0. This was a failed acknowledgement check.

The handler now validates the single SYNC packet and its event handles,
writes the eight-byte acknowledgement, and queues the synchronization work.
Each command signals its first event once, then waits for its second event.
Later commands stay ordered. Pending commands retain event references,
survive snapshots, and release references when completed or discarded.
Older snapshots preserve the following ASLR extension. Other nonempty command
formats and kernel-mode/broadcast submissions remain unsupported.

## Protocol references

- Guest d3d10warp.dll 10.0.19041.7181: FlushAllRenderingTasks RVA 0x99600;
  acknowledgement checks at 0x99C8F and 0x99C9B; RecordError RVA 0x349410.
- Local BasicRender.sys 10.0.19041.6216: WARPKMCONTEXT::Render RVA 0x491C;
  WARPGPUCMDSYNC constructor 0x5610, Run 0x5760, Discard 0x57C0. The reference
  driver and the captured guest agree on the 24-byte input and eight-byte reply.
- [D3DKMT_RENDER fields](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_render).

BasicRender reference SHA-256:
095debe32ae6690f75960b1bc271e4965982efaf985f78d97114c14fed7a4aea.

## Actual-game replay

Both builds restored the identical checkpoint before win32u.dll+0x5750.
The submitted command used event handles 0x1800070 and 0x1800071. Both calls
returned STATUS_SUCCESS to d3d11.dll+0x49C1E with RSP advanced by eight bytes.
The old buffer remained SYNC/24 and branched to WARP RVA 0x99DE4. The corrected
buffer became SYAK/8, preserved the handles and following bytes, and passed
both checks to RVA 0x99CA5. No guest registers or instructions were patched.

Local evidence: warp-render-old capture 1789413585309710900 and
warp-render-fixed capture 1789414453555881100, each with render-sync-proof.json.
Input snapshot SHA-256:
40e4b12d6ea233ec56321ebb327e4bf4f853f477b94b01ac5689b0082a292898.
The continuation executes beyond this failure; Sunrise boot is unconfirmed.

Release and tidy builds passed. Each passed 19 targeted tests in both
interpreter and JIT modes. These
include ordering, event lifetime, automatic-reset and notification behavior,
invalid packets, context destruction, pending snapshots, and legacy snapshots.
The CLI smoke passed 29 of 30 cases; its existing APIs failure remains.

The continuation capture 1789414522267486100 processed 85 further WARP SYNC
submissions and recorded ten thread exits with status zero. Its first
1,180,004,647 console bytes contain no logged unsupported operation or device
removal. This observation does not establish that startup completed.
