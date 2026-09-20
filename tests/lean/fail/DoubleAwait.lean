import LeanAT.Frontend.BlockSyntax
at_component Forbidden where
  process worker : Unit maxInstances 1 frameBytes 1024 results 1 do
    let timer ← registerWait until 5
    let first ← await timer
    let duplicate ← await timer
    return
