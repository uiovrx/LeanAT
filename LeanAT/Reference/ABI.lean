import LeanAT.Reference.Contract
namespace LeanAT.Reference.ABI
def le (n width : Nat) : ByteArray := ⟨(List.range width).toArray.map (fun i => UInt8.ofNat (n / 256^i % 256))⟩
def cat (xs : List ByteArray) : ByteArray := xs.foldl (· ++ ·) ByteArray.empty
def vector (f : α → ByteArray) (xs : List α) := le xs.length 4 ++ cat (xs.map f)
def str (s : String) := le s.toUTF8.size 4 ++ s.toUTF8
def word (n : Nat) := le n 4


private def roundConstants : Array UInt32 := #[
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]
private def rotr (x : UInt32) (n : UInt32) := (x >>> n) ||| (x <<< (32-n))
private def be (n width : Nat) : ByteArray := ⟨(List.range width).toArray.map (fun i => UInt8.ofNat (n / 256^(width-1-i) % 256))⟩

/-- SHA-256 over owned bytes; the artifact hash has no recursive self-reference. -/
def sha256 (bytes : ByteArray) : ByteArray := Id.run do
  let zeroCount := (64 - ((bytes.size + 9) % 64)) % 64
  let padded := bytes ++ le 128 1 ++ le 0 zeroCount ++ be (bytes.size*8) 8
  let mut hash : Array UInt32 := #[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]
  for block in [:padded.size / 64] do
    let mut w : Array UInt32 := #[]
    for i in [:16] do
      let offset := block*64+i*4
      let x := (List.range 4).foldl (fun n k => n*256+(padded[offset+k]!).toNat) 0
      w := w.push (UInt32.ofNat x)
    for i in [16:64] do
      let x := w[i-15]!
      let y := w[i-2]!
      let s0 := rotr x 7 ^^^ rotr x 18 ^^^ (x >>> 3)
      let s1 := rotr y 17 ^^^ rotr y 19 ^^^ (y >>> 10)
      w := w.push (w[i-16]! + s0 + w[i-7]! + s1)
    let mut a := hash[0]!
    let mut b := hash[1]!
    let mut c := hash[2]!
    let mut d := hash[3]!
    let mut e := hash[4]!
    let mut f := hash[5]!
    let mut g := hash[6]!
    let mut h := hash[7]!
    for i in [:64] do
      let s1 := rotr e 6 ^^^ rotr e 11 ^^^ rotr e 25
      let choice := (e &&& f) ^^^ (~~~e &&& g)
      let t1 := h + s1 + choice + roundConstants[i]! + w[i]!
      let s0 := rotr a 2 ^^^ rotr a 13 ^^^ rotr a 22
      let maj := (a &&& b) ^^^ (a &&& c) ^^^ (b &&& c)
      h := g; g := f; f := e; e := d+t1
      d := c; c := b; b := a; a := t1+s0+maj
    let working := #[a,b,c,d,e,f,g,h]
    hash := hash.mapIdx (fun i x => x+working[i]!)
  return cat (hash.toList.map (fun n => be n.toNat 4))

def hex (bytes : ByteArray) : String :=
  String.ofList (bytes.data.toList.flatMap (fun b =>
    let alphabet := "0123456789abcdef".toList
    [alphabet[b.toNat / 16]!,alphabet[b.toNat % 16]!]))

def handleKindTag : HandleKind → Nat
  | .transaction => 0 | .hop => 1 | .event => 2 | .process => 3 | .wait => 4 | .result => 5
  | .consumer => 6 | .scope => 7 | .task => 8 | .access => 9 | .lease => 10
  | .resourceTicket => 11 | .spawnTicket => 12 | .drain => 13 | .gateTicket => 14
def encodeType (t : TypeSchema) : ByteArray :=
  let (tag,bound,fields,cases) := match t with
    | .unit => (0,0,[],[]) | .bool => (1,0,[],[]) | .bits w => (2,w,[],[])
    | .fin n => (3,n,[],[]) | .record fs => (4,0,fs,[]) | .variant cs => (5,0,[],cs)
    | .vec t n => (6,n,[t],[]) | .boundedVec t n => (7,n,[t],[])
    | .bytes n => (8,n,[],[]) | .handle k => (9,handleKindTag k,[],[])
  le tag 1 ++ le bound 8 ++ vector word fields ++ vector (vector word) cases

end LeanAT.Reference.ABI

