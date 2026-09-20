#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
lake build LeanAT.Compiler.Main LeanAT.ExecIR.TimerReference LeanAT.Frontend.BlockSyntax LeanAT.ModelIR.Process
for file in Compiler Hierarchy ExecProcess SourceMap OpcodeCoverage ModelNodeCoverage; do
  lake env lean --run "tests/lean/${file}.lean"
done
lake env lean tests/lean/SourceProvenance.lean
