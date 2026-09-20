import LeanAT.Frontend.BlockSyntax
import LeanAT.Compiler.Main
set_option maxRecDepth 16384
set_option maxHeartbeats 4000000
open LeanAT

at_component Initiator where
  initiator bus : TlmBase 64 capacity 2 payload 8 mask 8

at_component Acceptor where
  target bus : TlmBase 64 capacity 2 payload 8 mask 8
  on bus.transport fw beginReq tx do
    returnTransport accepted

at_system TransportSystem where
  instance master := Initiator
  instance device := Acceptor
  bind master.bus => device.bus

def main (args : List String) : IO UInt32 := do
  match Compiler.compile TransportSystem.model with
  | .error e => IO.eprintln e; pure 1
  | .ok executable =>
    match args with
    | ["emit",path] => IO.FS.writeBinFile path (Compiler.serialize executable); pure 0
    | [] | ["check"] => IO.println "checked transport hierarchy"; pure 0
    | _ => Compiler.runCLI TransportSystem.model args TransportSystem.sourceBundle
