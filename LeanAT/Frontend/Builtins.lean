import LeanAT.ModelIR.Schema
namespace LeanAT.Frontend
/-- These identities name the trusted runtime implementations, not caller-supplied effects. -/
def processTypes : TypeEnvironment := [.unit,.bool,.bits 8,.bits 16,.bits 32,.bits 64,.handle .process,.handle .wait]
def timerServices : List ModelIR.ServiceIR := [
  {id:=0,opcode:="getContextField",inputTypes:=[],resultTypes:=[6],contextMask:=2,effectMask:=0,extraFuel:=1,providerKey:="leanat.core.context.process",providerVersion:="1",abiHash:=[243,98,119,232,11,86,30,127,47,204,190,94,31,222,10,169,182,89,145,83,240,145,48,189,31,28,150,241,33,51,160,218]},
  {id:=1,opcode:="registerWait",inputTypes:=[6,5,5],resultTypes:=[7],contextMask:=2,effectMask:=16,extraFuel:=1,providerKey:="leanat.core.wait.timer",providerVersion:="1",abiHash:=[120,61,218,156,158,167,189,34,238,43,43,7,82,48,225,16,33,150,50,251,70,56,134,174,209,87,149,131,210,174,163,111]}
]
def transportTypes : TypeEnvironment := processTypes ++ [.variant [[],[5]],.bytes 0,.record [5,9,1],.variant [[],[10]],.record [5,8,5,11]]
def acceptedTransport : Value := .record [.bits 64 0,.variant 0 [],.bits 64 0,.variant 0 []]
def transportService : ModelIR.ServiceIR := {id:=2,opcode:="setTransportReturn",inputTypes:=[12],resultTypes:=[],contextMask:=4,effectMask:=8,extraFuel:=1,providerKey:="leanat.core.transport.return",providerVersion:="1",abiHash:=[235,74,146,70,59,248,252,92,17,24,27,73,43,138,211,127,145,103,27,143,112,59,220,83,112,96,78,244,154,206,15,254]}
def outputService (typeId : Nat) : ModelIR.ServiceIR := {id:=100+typeId,opcode:="bufferOutputWrite",inputTypes:=[5,typeId],resultTypes:=[],contextMask:=3,effectMask:=4,extraFuel:=1,providerKey:="leanat.core.output.write",providerVersion:="1",abiHash:=[135,208,103,20,205,88,68,8,219,186,14,149,59,145,146,140,17,134,126,101,113,63,113,10,66,86,190,6,50,190,189,188]}
end LeanAT.Frontend
