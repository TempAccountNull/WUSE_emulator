# Graphics failure already stored in a checkpoint

Replaying the first texture call with commit c0f7faa1 still produced 68
CreateTexture2D failures, each returning 0x887A0005 with a null output and
balanced return stack. The subsequent Map would dereference a null resource.

Read-only inspection of the input checkpoint found the failure already stored
before that first texture call:

| Field | Address | Value |
| --- | --- | --- |
| Public ID3D11Device pointer | 0xBA5ADD7D98 | CDevice + 16 |
| NDXGI device pointer | 0xBA5ADD75A8 | Read from public pointer + 1144 |
| Cached removal reason | 0xBA5ADD7A60 | 0x887A0005 |
| Context removal flag | 0xBA5ADE28A0 | 0xFFFFFFFF |
| Resource dispatch target | d3d11.dll+0x33E10 | NOutermost::CDevice::CreateLayeredChild |

Offsets were checked against the guest d3d11.dll in IDA. The selected thread
and register packet were unchanged by the reads. This checkpoint cannot prove
that current resource-creation code causes the original device removal.

The earlier post-unpacking checkpoint has only three retained threads and
returns E01 when reading either later device-object address. The replay from
that checkpoint observes device-removal and UMD error callbacks from startup.
Absence at those addresses alone does not prove all earlier graphics state is
healthy. The original removal transition and Sunrise boot remain unverified.

Local evidence: graphics-api-returns capture 1789410624329191500 and the
read-only inspectors 1789411462539053600 and 1789411570549694200. The texture
checkpoint SHA-256 is
cf410be533e0a31d8abd6901c13bf4a9bb23edca623764417bda0fcfeaf41f11.
