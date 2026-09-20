import LeanAT.Frontend.BlockSyntax
set_option maxRecDepth 8192
set_option maxHeartbeats 2000000
open LeanAT LeanAT.Frontend

at_component Signals where
  input reset_n : Bool
  output irq : Bool := false
  on internal.raise event do
    set output irq := true
    return

at_system SignalSystem where
  input reset_n : Bool := true
  output irq : Bool
  instance device := Signals
  connect top.reset_n => device.reset_n
  connect device.irq => top.irq

#guard (ModelIR.validateSchema SignalSystem.model).isOk
#guard (SignalSystem.model.systems.head?.map (fun s => s.sidebandBindings.length)) == some 2
example : checkSystem SignalSystem.topology = .ok () := by rfl
example : checkSystem {SignalSystem.topology with sidebandBindings := []} = .error "UndrivenTopOutput" := by rfl
example : checkSystem {SignalSystem.topology with sidebandBindings := [(.top 0,.instance ⟨0⟩ 0),(.top 0,.instance ⟨0⟩ 0)]} = .error "MultipleSidebandDrivers" := by rfl
example : checkSystem {SignalSystem.topology with sidebandBindings := [(.instance ⟨0⟩ 0,.top 0)]} = .error "SidebandDirection" := by rfl
example : checkSidebands {SignalSystem.topology with topPorts := [{name := "bad",direction:=.input,typeId:=0,initial:=some .unit}]} = .error "InvalidTopPortType" := by rfl
