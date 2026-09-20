"""Convert an actual closed-segment backend observation into a benchmark report."""
import argparse
from pathlib import Path
from .cli import capture
from .io import canonical,digest,write_json,read_json,ToolError


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--backend",required=True)
    parser.add_argument("--descriptor",required=True)
    parser.add_argument("--program",type=int,required=True)
    parser.add_argument("--fuel",type=int,required=True)
    parser.add_argument("--report",required=True)
    parser.add_argument("--fixture",required=True)
    args=parser.parse_args()
    fixture=read_json(args.fixture)
    descriptor_hash=digest(Path(args.descriptor).read_bytes())
    if fixture.get("artifacts",{}).get("descriptorHash")!=descriptor_hash or fixture.get("segment",{}).get("programId")!=args.program or fixture.get("segment",{}).get("inputs")!=[] or fixture.get("seed")!="0":raise ToolError("ConfigurationMismatch","closed scalar benchmark fixture mismatch")
    result,execution=capture([args.backend,"--run",args.descriptor,str(args.program),str(args.fuel)])
    observation={"trace":[{"kind":"commit","state":result["state"]},{"kind":"output","result":result["result"]}],"feedback":[],"complete":result["complete"],"stopReason":result["stopReason"],"observationEndpoint":"scalar-segment-return","dataMode":"FullValue","transcriptHash":digest(canonical({"descriptorHash":digest(Path(args.descriptor).read_bytes()),"programId":args.program,"inputs":[]})),"execution":execution}
    observation.update(benchmarkInput=fixture,seed="0",successDefinition="scalar-segment-completed-and-equal-v1",buildHash=digest(Path(args.backend).read_bytes()))
    write_json(args.report,observation)
    return execution["exitCode"]


if __name__=="__main__":raise SystemExit(main())
