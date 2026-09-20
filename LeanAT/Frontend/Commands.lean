import Lean
import LeanAT.Frontend.Checks

open Lean Elab Command
namespace LeanAT.Frontend

/-- An elaborator-produced check record, deliberately not a semantic theorem. -/
structure CheckedDeclaration (α : Type) where
  val : α
  checkerVersion : Nat := 2

def enforceCheck (result : Except String Unit) : IO Unit :=
  match result with
  | .ok () => pure ()
  | .error message => throw (IO.userError message)

private def runChecker (checkerExpr : TSyntax `term) : CommandElabM Unit := do
  elabCommand (← `(#eval LeanAT.Frontend.enforceCheck $checkerExpr))
  if (← get).messages.hasErrors then throwError "AT checker failed; declaration not published"

/-- The four registered declaration commands accept explicit, finite typed records.
Unsupported v0.4 block forms are parser errors rather than ignored statements. -/
syntax (name := atComponent) "at_component " ident " := " term : command
syntax (name := atSystem) "at_system " ident " := " term : command
syntax (name := atExtern) "at_extern_component " ident " := " term : command
syntax (name := atProtocol) "at_protocol " ident " profile " ident " := " term : command

private def sourceTerm (stx : Syntax) : CommandElabM (TSyntax `term) := do
  let file ← getFileName
  let map ← getFileMap
  let pos := map.toPosition (stx.getPos?.getD 0)
  `(LeanAT.SourceSpan.mk $(quote file) $(quote pos.line) $(quote (pos.column + 1)))

@[command_elab atComponent] def elabComponent : CommandElab := fun stx => do
  match stx with
  | `(at_component $name:ident := $body:term) =>
    let source ← sourceTerm stx
    let checked := mkIdent (name.getId ++ `checked)
    let model := mkIdent (name.getId ++ `model)
    let value ← `(LeanAT.Frontend.withSource $body $source)
    runChecker (← `(LeanAT.Frontend.checkComponent $value))
    elabCommand (← `(def $checked : LeanAT.Frontend.CheckedDeclaration LeanAT.Frontend.Component := ⟨$value,2⟩))
    elabCommand (← `(def $model : LeanAT.ModelIR.Project := LeanAT.Frontend.componentProject ($checked).val))
  | _ => throwUnsupportedSyntax

@[command_elab atSystem] def elabSystem : CommandElab := fun stx => do
  match stx with
  | `(at_system $name:ident := $body:term) =>
    let source ← sourceTerm stx
    let checked := mkIdent (name.getId ++ `checked)
    let model := mkIdent (name.getId ++ `model)
    let topology := mkIdent (name.getId ++ `topology)
    let value ← `(({ ($body : LeanAT.Frontend.System) with source := $source } : LeanAT.Frontend.System))
    runChecker (← `(LeanAT.Frontend.checkSystem $value))
    elabCommand (← `(def $checked : LeanAT.Frontend.CheckedDeclaration LeanAT.Frontend.System := ⟨$value,2⟩))
    elabCommand (← `(def $topology : LeanAT.Frontend.System := ($checked).val))
    elabCommand (← `(def $model : LeanAT.ModelIR.Project := LeanAT.Frontend.exportSystem ($checked).val))
  | _ => throwUnsupportedSyntax

@[command_elab atExtern] def elabExtern : CommandElab := fun stx => do
  match stx with
  | `(at_extern_component $name:ident := $body:term) =>
    let source ← sourceTerm stx
    let checked := mkIdent (name.getId ++ `checked)
    let model := mkIdent (name.getId ++ `model)
    let component := mkIdent (name.getId ++ `component)
    let value ← `(LeanAT.Frontend.withExternSource $body $source)
    runChecker (← `(LeanAT.Frontend.checkExtern $value))
    elabCommand (← `(def $checked : LeanAT.Frontend.CheckedDeclaration LeanAT.Frontend.ExternComponent := ⟨$value,2⟩))
    elabCommand (← `(def $component : LeanAT.Frontend.Component := ($checked).val.component))
    elabCommand (← `(def $model : LeanAT.ModelIR.Project := LeanAT.Frontend.exportExtern ($checked).val))
  | _ => throwUnsupportedSyntax

@[command_elab atProtocol] def elabProtocol : CommandElab := fun stx => do
  match stx with
  | `(at_protocol $name:ident profile $prof:ident := $body:term) =>
    if prof.getId != `ext then throwErrorAt prof "MissingCapability: at_protocol requires explicit profile ext"
    let checked := mkIdent (name.getId ++ `checked)
    let model := mkIdent (name.getId ++ `model)
    let source ← sourceTerm stx
    let value ← `(({ ($body : LeanAT.Protocol.Package) with sourceFile := ($source).file, sourceLine := ($source).line, sourceColumn := ($source).column } : LeanAT.Protocol.Package))
    runChecker (← `(LeanAT.Protocol.validate $value))
    elabCommand (← `(def $checked : LeanAT.Frontend.CheckedDeclaration LeanAT.Protocol.Package := ⟨$value,2⟩))
    elabCommand (← `(def $model : LeanAT.Protocol.Package := ($checked).val))
  | _ => throwUnsupportedSyntax

end LeanAT.Frontend





