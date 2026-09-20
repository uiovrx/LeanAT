import Lake
open Lake DSL
package leanat where
@[default_target]
lean_lib LeanAT where

lean_lib LeanATConformance where
  roots := #[`tests.conformance.ScenarioCatalog]
  globs := #[.submodules `tests.conformance]

