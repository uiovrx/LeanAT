import LeanAT.Frontend.Commands
import LeanAT.Protocol.Builtins
set_option maxRecDepth 8192
set_option maxHeartbeats 1000000
at_protocol Probe profile ext := LeanAT.Protocol.ProbeV1
at_protocol Marker profile ext := LeanAT.Protocol.TraceMarkerV1
#eval Probe.model.rules.length

