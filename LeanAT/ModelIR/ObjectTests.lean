import LeanAT.ModelIR.Object
namespace LeanAT.ModelIR.Object.Tests
#guard memoryTransfer [1,2,3] ⟨.ignore,2^90,[],0,[]⟩ 0 == ⟨[1,2,3],[],.ok⟩
#guard memoryTransfer [1,2,3] ⟨.write,2,[9,8],2,[255,0]⟩ 10 == ⟨[1,2,3],[9,8],.addressError⟩
#guard memoryTransfer [1,2,3,4,5] ⟨.read,2,[99,99,99,99,99,99],3,[255,0]⟩ 10 == ⟨[1,2,3,4,5],[3,99,5,99,4,99],.ok⟩
#guard memoryTransfer [0,0] ⟨.write,0,[1,2,3,4],2,[]⟩ 10 == ⟨[3,4],[1,2,3,4],.ok⟩
#guard memoryTransfer [0,0] ⟨.write,0,[1,2],2,[1]⟩ 10 == ⟨[0,0],[1,2],.byteEnableError⟩
#guard memoryDebug [1,2,3] ⟨.read,1,[99,99,99],0,[]⟩ 1 == ⟨[1,2,3],[2,99,99],1⟩
#guard match queuePush [.bits 8] ⟨1,0,[.bits 8 9]⟩ (.bits 8 1) with | .ok (s,b) => !b && s.values == [.bits 8 9] | _ => false
#guard queuePop ⟨2,0,[.bits 8 1,.bits 8 2]⟩ == (⟨2,0,[.bits 8 2]⟩,some (.bits 8 1))
#guard match resourceReserve ⟨2,4,2,true⟩ {channelAvailable := [0,0]} 0 0 100 10 with
  | .error _ => false
  | .ok (s,g) => match resourceReserve ⟨2,4,2,true⟩ s 1 1 1 10 with
    | .error _ => false
    | .ok (_,second) => g.start == 100 && g.channel == 0 && second.start == 102 && second.channel == 1
#guard match resourceReserve ⟨1,1,0,false⟩ {channelAvailable := [0]} 0 0 (2^64-1) 1 with | .error "TimeOverflow" => true | _ => false
end LeanAT.ModelIR.Object.Tests

