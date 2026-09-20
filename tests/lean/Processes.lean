import LeanAT.Frontend.BlockSyntax
import LeanAT.Compiler.Lowering
set_option maxRecDepth 16384
set_option maxHeartbeats 4000000
open LeanAT LeanAT.Frontend
example : checkBody .timed 20 [.await (.local 0 0) 1 0] = .error "AwaitForbiddenInContext" := by rfl
example : checkBody .transport 20 [.await (.local 0 0) 1 0] = .error "AwaitForbiddenInContext" := by rfl
example : checkBodyWithServices timerServices .timed 20 [.serviceCall (some ⟨0,7,{}⟩) 1 []] = .error "ServiceForbiddenInContext" := by rfl
def taskSlotEffectFixture : ModelIR.ServiceIR := {id:=9,opcode:="requestTaskSlot",inputTypes:=[],resultTypes:=[],contextMask:=2,effectMask:=32,extraFuel:=1,providerKey:="test.effect.requestTaskSlot",providerVersion:="1",abiHash:=List.replicate 32 0}
example : checkBodyWithServices [taskSlotEffectFixture] .timed 20 [.serviceCall none 9 [],.ret []] = .error "ServiceForbiddenInContext" := by rfl
example : checkBodyWithServices [taskSlotEffectFixture] .process 20 [.serviceCall none 9 [],.ret []] = .ok true := by rfl

at_component ExplicitWait where
  process worker : Unit maxInstances 1 frameBytes 1024 results 1 do
    let timer ← registerWait until 5
    let outcome ← await timer
    return

#guard (ModelIR.validateSchema ExplicitWait.model).isOk
#guard (Compiler.compile ExplicitWait.model).isOk
