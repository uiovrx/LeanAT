import LeanAT.ModelIR.Schema
namespace LeanAT.Frontend
def listEqWith (eq : α → α → Bool) (a b : List α) : Bool :=
  a.length == b.length && (a.zip b).all (fun (x,y) => eq x y)
def valueEq : Nat → Value → Value → Bool
  | 0, _, _ => false
  | _+1, .unit, .unit => true
  | _+1, .bool a, .bool b => a == b
  | _+1, .bits a b, .bits c d => a == c && b == d
  | f+1, .record a, .record b | f+1, .vec a, .vec b => listEqWith (valueEq f) a b
  | f+1, .variant t a, .variant u b => t == u && listEqWith (valueEq f) a b
  | _+1, .bytes a, .bytes b => a == b
  | _+1, .handle a, .handle b => a == b
  | _, _, _ => false
def exprEq : Nat → ModelIR.Expr → ModelIR.Expr → Bool
  | 0, _, _ => false
  | f+1, .literal t a, .literal u b => t == u && valueEq f a b
  | _+1, .local t a, .local u b | _+1, .state t a, .state u b => t == u && a == b
  | f+1, .select t a b c, .select u d e g => t == u && exprEq f a d && exprEq f b e && exprEq f c g
  | f+1, .binary t op a b, .binary u oq c d => t == u && op == oq && exprEq f a c && exprEq f b d
  | f+1, .field t a n, .field u b m => t == u && n == m && exprEq f a b
  | f+1, .index t a b, .index u c d => t == u && exprEq f a c && exprEq f b d
  | f+1, .makeRecord t a, .makeRecord u b => t == u && listEqWith (exprEq f) a b
  | f+1, .makeVariant t tag a, .makeVariant u tag' b => t == u && tag == tag' && listEqWith (exprEq f) a b
  | _, _, _ => false
def stmtEq : Nat → ModelIR.Stmt → ModelIR.Stmt → Bool
  | 0, _, _ => false
  | _+1, .readNow a, .readNow b => a.id == b.id && a.typeId == b.typeId
  | f+1, .letVal n a, .letVal m b | f+1, .writeState n a, .writeState m b => n == m && exprEq f a b
  | f+1, .check a s, .check b t => s == t && exprEq f a b
  | f+1, .branch c a b, .branch d e g => exprEq f c d && listEqWith (stmtEq f) a e && listEqWith (stmtEq f) b g
  | f+1, .repeat n a, .repeat m b => n == m && listEqWith (stmtEq f) a b
  | f+1, .emit s a, .emit t b => s == t && listEqWith (exprEq f) a b
  | _+1, .fail s, .fail t | _+1, .unsupported s, .unsupported t => s == t
  | f+1, .ret a, .ret b => listEqWith (exprEq f) a b
  | f+1, .transportReturn a, .transportReturn b => exprEq f a b
  | f+1, .serviceCall a id xs, .serviceCall b id' ys => a == b && id == id' && listEqWith (exprEq f) xs ys
  | f+1, .await a id t, .await b id' t' => id == id' && t == t' && exprEq f a b
  | _, _, _ => false
end LeanAT.Frontend
