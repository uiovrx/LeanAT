import LeanAT.Frontend.BlockSyntax
at_component Forbidden where
  process worker : Unit maxInstances 1 frameBytes 128 results 1 do
    return
    emit "late"
