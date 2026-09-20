import LeanAT.Frontend.BlockSyntax
at_component Forbidden where
  target bus : TlmBase 64 capacity 1 payload 8 mask 8
  on bus.beginReq tx do
    await response
    return
