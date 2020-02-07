(set-info :source |fuzzsmt|)
(set-info :smt-lib-version 2.0)
(set-info :category "random")
(set-info :status unknown)
(set-logic QF_BV)
(declare-fun v0 () (_ BitVec 7))
(declare-fun v1 () (_ BitVec 8))
(assert (let ((e2(_ bv0 1)))
(let ((e3(_ bv2 2)))
(let ((e4 (bvor v1 ((_ zero_extend 7) e2))))
(let ((e5 (bvashr v0 ((_ sign_extend 6) e2))))
(let ((e6 (bvsrem ((_ zero_extend 5) e3) e5)))
(let ((e7 (= e6 e5)))
(let ((e8 (bvsge e4 ((_ zero_extend 1) e5))))
(let ((e9 (bvsle v1 ((_ zero_extend 7) e2))))
(let ((e10 (bvult ((_ sign_extend 6) e2) e5)))
(let ((e11 (bvule ((_ zero_extend 5) e3) e5)))
(let ((e12 (distinct ((_ zero_extend 6) e2) e6)))
(let ((e13 (bvsgt v1 e4)))
(let ((e14 (bvuge e6 ((_ zero_extend 5) e3))))
(let ((e15 (bvsle ((_ zero_extend 7) e2) e4)))
(let ((e16 (bvslt ((_ sign_extend 1) v0) e4)))
(let ((e17 (not e7)))
(let ((e18 (= e14 e16)))
(let ((e19 (xor e18 e11)))
(let ((e20 (=> e17 e13)))
(let ((e21 (= e12 e12)))
(let ((e22 (=> e9 e10)))
(let ((e23 (xor e22 e15)))
(let ((e24 (= e21 e23)))
(let ((e25 (or e24 e20)))
(let ((e26 (= e8 e19)))
(let ((e27 (or e26 e26)))
(let ((e28 (not e27)))
(let ((e29 (= e25 e28)))
(let ((e30 (and e29 (not (= e5 (_ bv0 7))))))
(let ((e31 (and e30 (not (= e5 (bvnot (_ bv0 7)))))))
e31
)))))))))))))))))))))))))))))))

(check-sat)