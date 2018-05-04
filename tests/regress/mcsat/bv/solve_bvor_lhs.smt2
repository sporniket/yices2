(set-logic QF_BV)
(set-info :status sat)

(declare-fun c () (_ BitVec 8))
(declare-fun s () (_ BitVec 8))
(declare-fun x () (_ BitVec 8))

(assert (= c #b01010111))
(assert (= s #b11010111))
(assert (= (bvor x c) s))

(check-sat)
(get-model)

(exit)
