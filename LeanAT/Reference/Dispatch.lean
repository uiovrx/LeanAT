import LeanAT.Reference.Runtime
import LeanAT.Reference.Structured
import LeanAT.Reference.Objects
import LeanAT.Reference.Managed
import LeanAT.Reference.Protocol
namespace LeanAT.Reference.Dispatch
private def familyCount (service : ExecIR.ServiceSignature) : Nat :=
  ([Runtime.supports service,Storage.supports service,Structured.supports service,Objects.supports service,Managed.supports service,Protocol.supports service].filter id).length

def validate (types : TypeEnvironment) (context : Context) (services : List ExecIR.ServiceSignature) : Except String Unit := do
  checkContext context
  for service in services do
    if familyCount service != 1 then throw "ReferenceProviderDispatchAmbiguousOrMissing"
    if Runtime.supports service then Runtime.validate types service
    else if Storage.supports service then Storage.validate types service
    else if Structured.supports service then Structured.validate types service
    else if Objects.supports service then Objects.validate types service
    else if Protocol.supports service then Protocol.validate types service
    else Managed.validateEnvironment types context service

def invokeAttempt (types : TypeEnvironment) (context : Context) (service : ExecIR.ServiceSignature) (args : List Value) : Attempt (List Value) := do
  fromExcept (validate types context [service])
  if Nat.land service.contextMask (2^context.kind) == 0 then throw "ReferenceServiceContext"
  if args.length != service.inputTypes.length then throw "ReferenceServiceInputArity"
  for (typeId,value) in service.inputTypes.zip args do
    if !(types[typeId]?).any (fun type => conforms types (types.length+1) type value) then throw "ReferenceServiceInputType"
  let values ← if Runtime.supports service then Runtime.invokeAttempt types context service args
    else if Storage.supports service then Storage.invokeAttempt types context service args
    else if Structured.supports service then Structured.invokeAttempt types context service args
    else if Objects.supports service then Objects.invokeAttempt types context service args
    else if Protocol.supports service then do
      let (result,draft) := Protocol.attempt types context service args (← get)
      set draft
      fromExcept result
    else Managed.invokeAttempt types context service args
  if values.length != service.resultTypes.length then throw "ReferenceServiceResultArity"
  for (typeId,value) in service.resultTypes.zip values do
    if !(types[typeId]?).any (fun type => conforms types (types.length+1) type value) then throw "ReferenceServiceResultType"
  fromExcept (checkCapacity (← get))
  pure values

def attempt (types : TypeEnvironment) (context : Context) (service : ExecIR.ServiceSignature) (args : List Value) (state : State) : EStateM.Result String State (List Value) :=
  (invokeAttempt types context service args).run state

def invoke (types : TypeEnvironment) (context : Context) (service : ExecIR.ServiceSignature) (args : List Value) (state : State) : Except String (List Value × State) :=
  attemptExcept (attempt types context service args state)
end LeanAT.Reference.Dispatch
