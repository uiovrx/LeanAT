import LeanAT.Frontend.BlockSyntax
at_component Forbidden where
  on internal.tick event do
    awaitSlot workers
    return
