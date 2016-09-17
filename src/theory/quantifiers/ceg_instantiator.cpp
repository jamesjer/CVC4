/*********************                                                        */
/*! \file ceg_instantiator.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Andrew Reynolds, Tim King
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2016 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief Implementation of counterexample-guided quantifier instantiation
 **/
#include "theory/quantifiers/ceg_instantiator.h"

#include "options/quantifiers_options.h"
#include "smt/ite_removal.h"
#include "theory/arith/partial_model.h"
#include "theory/arith/theory_arith.h"
#include "theory/arith/theory_arith_private.h"
#include "theory/quantifiers/first_order_model.h"
#include "theory/quantifiers/term_database.h"
#include "theory/quantifiers/quantifiers_rewriter.h"
#include "theory/quantifiers/trigger.h"
#include "theory/theory_engine.h"
#include "theory/quantifiers/term_database.h"

#include "theory/bv/theory_bv_utils.h"
#include "util/bitvector.h"

//#define MBP_STRICT_ASSERTIONS

using namespace std;
using namespace CVC4;
using namespace CVC4::kind;
using namespace CVC4::context;
using namespace CVC4::theory;
using namespace CVC4::theory::quantifiers;

CegInstantiator::CegInstantiator( QuantifiersEngine * qe, CegqiOutput * out, bool use_vts_delta, bool use_vts_inf ) :
d_qe( qe ), d_out( out ), d_use_vts_delta( use_vts_delta ), d_use_vts_inf( use_vts_inf ){
  d_zero = NodeManager::currentNM()->mkConst( Rational( 0 ) );
  d_one = NodeManager::currentNM()->mkConst( Rational( 1 ) );
  d_true = NodeManager::currentNM()->mkConst( true );
  d_is_nested_quant = false;
}

CegInstantiator::~CegInstantiator() {
  for( std::map< Node, Instantiator * >::iterator it = d_instantiator.begin(); it != d_instantiator.end(); ++it ){
    delete it->second;
  }
}

void CegInstantiator::computeProgVars( Node n ){
  if( d_prog_var.find( n )==d_prog_var.end() ){
    d_prog_var[n].clear();
    if( std::find( d_vars.begin(), d_vars.end(), n )!=d_vars.end() ){
      d_prog_var[n][n] = true;
    }else if( !d_out->isEligibleForInstantiation( n ) ){
      d_inelig[n] = true;
      return;
    }
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      computeProgVars( n[i] );
      if( d_inelig.find( n[i] )!=d_inelig.end() ){
        d_inelig[n] = true;
        return;
      }
      for( std::map< Node, bool >::iterator it = d_prog_var[n[i]].begin(); it != d_prog_var[n[i]].end(); ++it ){
        d_prog_var[n][it->first] = true;
      }
      //selectors applied to program variables are also variables
      if( n.getKind()==APPLY_SELECTOR_TOTAL && d_prog_var[n].find( n[0] )!=d_prog_var[n].end() ){
        d_prog_var[n][n] = true;
      }
    }
  }
}

bool CegInstantiator::isEligible( Node n ) {
  //compute d_subs_fv, which program variables are contained in n, and determines if eligible
  computeProgVars( n );
  return d_inelig.find( n )==d_inelig.end();
}

void CegInstantiator::registerInstantiationVariable( Node v, unsigned index ) {
  if( d_instantiator.find( v )==d_instantiator.end() ){
    TypeNode tn = v.getType();
    Instantiator * vinst;
    if( tn.isReal() ){
      vinst = new ArithInstantiator( d_qe, tn );
    }else if( tn.isSort() ){
      Assert( options::quantEpr() );
      vinst = new EprInstantiator( d_qe, tn );
    }else if( tn.isDatatype() ){
      vinst = new DtInstantiator( d_qe, tn );
    }else if( tn.isBitVector() ){
      vinst = new BvInstantiator( d_qe, tn );
    }else if( tn.isBoolean() ){
      vinst = new ModelValueInstantiator( d_qe, tn );
    }else{
      //default
      vinst = new Instantiator( d_qe, tn );
    }
    d_instantiator[v] = vinst;
  }
  d_curr_subs_proc[v].clear();
  d_curr_index[v] = index;
}

void CegInstantiator::unregisterInstantiationVariable( Node v ) {
  d_curr_subs_proc.erase( v );
  d_curr_index.erase( v );
}

bool CegInstantiator::doAddInstantiation( SolvedForm& sf, unsigned i, unsigned effort ){
  if( i==d_vars.size() ){
    //solved for all variables, now construct instantiation
    bool needsPostprocess = !sf.d_has_coeff.empty();    
    if( needsPostprocess ){
      //must make copy so that backtracking reverts sf
      SolvedForm sf_tmp;
      sf_tmp.copy( sf );
      bool postProcessSuccess = true;
      if( !processInstantiationCoeff( sf_tmp ) ){
        postProcessSuccess = false;
      }  
      if( postProcessSuccess ){
        return doAddInstantiation( sf_tmp.d_subs, sf_tmp.d_vars );
      }else{
        return false;
      }
    }else{
      return doAddInstantiation( sf.d_subs, sf.d_vars );
    }
  }else{
    //Node v = d_single_inv_map_to_prog[d_single_inv[0][i]];
    bool is_cv = false;
    Node pv;
    if( d_stack_vars.empty() ){
      pv = d_vars[i];
    }else{
      pv = d_stack_vars.back();
      is_cv = true;
      d_stack_vars.pop_back();
    }
    registerInstantiationVariable( pv, i );

    //get the instantiator object
    Instantiator * vinst = NULL;
    std::map< Node, Instantiator * >::iterator itin = d_instantiator.find( pv );
    if( itin!=d_instantiator.end() ){
      vinst = itin->second;
    }
    Assert( vinst!=NULL );
    d_active_instantiators[vinst] = true;
    vinst->reset( pv, effort );

    TypeNode pvtn = pv.getType();
    TypeNode pvtnb = pvtn.getBaseType();
    Node pvr = pv;
    if( d_qe->getMasterEqualityEngine()->hasTerm( pv ) ){
      pvr = d_qe->getMasterEqualityEngine()->getRepresentative( pv );
    }
    Trace("cbqi-inst-debug") << "[Find instantiation for " << pv << "], rep=" << pvr << ", instantiator is " << vinst->identify() << std::endl;
    Node pv_value;
    if( options::cbqiModel() ){
      pv_value = getModelValue( pv );
      Trace("cbqi-bound2") << "...M( " << pv << " ) = " << pv_value << std::endl;
    }

    //if in effort=2, we must choose at least one model value
    if( (i+1)<d_vars.size() || effort!=2 ){

      //[1] easy case : pv is in the equivalence class as another term not containing pv
      Trace("cbqi-inst-debug") << "[1] try based on equivalence class." << std::endl;
      std::map< Node, std::vector< Node > >::iterator it_eqc = d_curr_eqc.find( pvr );
      if( it_eqc!=d_curr_eqc.end() ){
        //std::vector< Node > eq_candidates;
        Trace("cbqi-inst-debug2") << "...eqc has size " << it_eqc->second.size() << std::endl;
        for( unsigned k=0; k<it_eqc->second.size(); k++ ){
          Node n = it_eqc->second[k];
          if( n!=pv ){
            Trace("cbqi-inst-debug") << "...try based on equal term " << n << std::endl;
            //must be an eligible term
            if( isEligible( n ) ){
              Node ns;
              Node pv_coeff;  //coefficient of pv in the equality we solve (null is 1)
              bool proc = false;
              if( !d_prog_var[n].empty() ){
                ns = applySubstitution( pvtn, n, sf, pv_coeff, false );
                if( !ns.isNull() ){
                  computeProgVars( ns );
                  //substituted version must be new and cannot contain pv
                  proc = d_prog_var[ns].find( pv )==d_prog_var[ns].end();
                }
              }else{
                ns = n;
                proc = true;
              }
              if( proc ){
                if( vinst->processEqualTerm( this, sf, pv, pv_coeff, ns, effort ) ){
                  return true;
                }
                //try the substitution
                //if( doAddInstantiationInc( pv, ns, pv_coeff, 0, sf, effort ) ){
                //  return true;
                //}
              }
            }
          }
        }
        if( vinst->processEqualTerms( this, sf, pv, it_eqc->second, effort ) ){
          return true;
        }
      }else{
        Trace("cbqi-inst-debug2") << "...eqc not found." << std::endl;
      }

      //[3] : we can solve an equality for pv
      ///iterate over equivalence classes to find cases where we can solve for the variable
      Trace("cbqi-inst-debug") << "[3] try based on solving equalities." << std::endl;
      for( unsigned k=0; k<d_curr_type_eqc[pvtnb].size(); k++ ){
        Node r = d_curr_type_eqc[pvtnb][k];
        std::map< Node, std::vector< Node > >::iterator it_reqc = d_curr_eqc.find( r );
        std::vector< Node > lhs;
        std::vector< bool > lhs_v;
        std::vector< Node > lhs_coeff;
        Assert( it_reqc!=d_curr_eqc.end() );
        for( unsigned kk=0; kk<it_reqc->second.size(); kk++ ){
          Node n = it_reqc->second[kk];
          Trace("cbqi-inst-debug2") << "...look at term " << n << std::endl;
          //must be an eligible term
          if( isEligible( n ) ){
            Node ns;
            Node pv_coeff;
            if( !d_prog_var[n].empty() ){
              ns = applySubstitution( pvtn, n, sf, pv_coeff );
              if( !ns.isNull() ){
                computeProgVars( ns );
              }
            }else{
              ns = n;
            }
            if( !ns.isNull() ){
              bool hasVar = d_prog_var[ns].find( pv )!=d_prog_var[ns].end();
              Trace("cbqi-inst-debug2") << "... " << ns << " has var " << pv << " : " << hasVar << std::endl;
              //std::vector< Node > term_coeffs;
              //std::vector< Node > terms;
              //term_coeffs.push_back( pv_coeff );
              //terms.push_back( ns );
              for( unsigned j=0; j<lhs.size(); j++ ){
                //if this term or the another has pv in it, try to solve for it
                if( hasVar || lhs_v[j] ){
                  Trace("cbqi-inst-debug") << "... " << i << "...try based on equality " << lhs[j] << " = " << ns << std::endl;
                  Node val;
                  Node veq_c;
                  if( pvtnb.isReal() ){
                    Node eq_lhs = lhs[j];
                    Node eq_rhs = ns;
                    //make the same coefficient
                    if( pv_coeff!=lhs_coeff[j] ){
                      if( !pv_coeff.isNull() ){
                        Trace("cbqi-inst-debug") << "...mult lhs by " << pv_coeff << std::endl;
                        eq_lhs = NodeManager::currentNM()->mkNode( MULT, pv_coeff, eq_lhs );
                        eq_lhs = Rewriter::rewrite( eq_lhs );
                      }
                      if( !lhs_coeff[j].isNull() ){
                        Trace("cbqi-inst-debug") << "...mult rhs by " << lhs_coeff[j] << std::endl;
                        eq_rhs = NodeManager::currentNM()->mkNode( MULT, lhs_coeff[j], eq_rhs );
                        eq_rhs = Rewriter::rewrite( eq_rhs );
                      }
                    }
                    Node eq = eq_lhs.eqNode( eq_rhs );
                    eq = Rewriter::rewrite( eq );
                    Node vts_coeff_inf;
                    Node vts_coeff_delta;
                    //isolate pv in the equality
                    int ires = solve_arith( pv, eq, veq_c, val, vts_coeff_inf, vts_coeff_delta );
                    if( ires!=0 ){
                      if( doAddInstantiationInc( pv, val, veq_c, 0, sf, effort ) ){
                        return true;
                      }
                    }
                  }else if( pvtnb.isDatatype() ){
                    val = solve_dt( pv, lhs[j], ns, lhs[j], ns );
                    if( !val.isNull() ){
                      if( doAddInstantiationInc( pv, val, veq_c, 0, sf, effort ) ){
                        return true;
                      }
                    }
                  }
                }
              }
              lhs.push_back( ns );
              lhs_v.push_back( hasVar );
              lhs_coeff.push_back( pv_coeff );
            }else{
              Trace("cbqi-inst-debug2") << "... term " << n << " is ineligible after substitution." << std::endl;
            }
          }else{
            Trace("cbqi-inst-debug2") << "... term " << n << " is ineligible." << std::endl;
          }
        }
      }

      //[4] directly look at assertions
      if( vinst->hasProcessAssertion( this, sf, pv, effort ) ){
        Trace("cbqi-inst-debug") << "[4] try based on assertions." << std::endl;
        d_vts_sym[0] = d_qe->getTermDatabase()->getVtsInfinity( pvtn, false, false );
        d_vts_sym[1] = d_qe->getTermDatabase()->getVtsDelta( false, false );
        std::vector< Node > mbp_bounds[2];
        std::vector< Node > mbp_coeff[2];
        std::vector< Node > mbp_vts_coeff[2][2];
        std::vector< Node > mbp_lit[2];
        std::vector< Node > lits;
        //unsigned rmax = Theory::theoryOf( pv )==Theory::theoryOf( pv.getType() ) ? 1 : 2;
        for( unsigned r=0; r<2; r++ ){
          TheoryId tid = r==0 ? Theory::theoryOf( pvtn ) : THEORY_UF;
          Trace("cbqi-inst-debug2") << "  look at assertions of " << tid << std::endl;
          std::map< TheoryId, std::vector< Node > >::iterator ita = d_curr_asserts.find( tid );
          if( ita!=d_curr_asserts.end() ){
            for (unsigned j = 0; j<ita->second.size(); j++) {
              Node lit = ita->second[j];
              if( std::find( lits.begin(), lits.end(), lit )==lits.end() ){
                lits.push_back( lit );
                if( vinst->processAssertion( this, sf, pv, lit, effort ) ){
                  return true;
                }
                  
                
                Trace("cbqi-inst-debug2") << "  look at " << lit << std::endl;
                Node atom = lit.getKind()==NOT ? lit[0] : lit;
                bool pol = lit.getKind()!=NOT;
                if( pvtn.isReal() ){
                  //arithmetic inequalities and disequalities
                  if( atom.getKind()==GEQ || ( atom.getKind()==EQUAL && !pol && atom[0].getType().isReal() ) ){
                    Assert( atom.getKind()!=GEQ || atom[1].isConst() );
                    Node atom_lhs;
                    Node atom_rhs;
                    if( atom.getKind()==GEQ ){
                      atom_lhs = atom[0];
                      atom_rhs = atom[1];
                    }else{
                      atom_lhs = NodeManager::currentNM()->mkNode( MINUS, atom[0], atom[1] );
                      atom_lhs = Rewriter::rewrite( atom_lhs );
                      atom_rhs = d_zero;
                    }
                    //must be an eligible term
                    if( isEligible( atom_lhs ) ){
                      //apply substitution to LHS of atom
                      if( !d_prog_var[atom_lhs].empty() ){
                        Node atom_lhs_coeff;
                        atom_lhs = applySubstitution( pvtn, atom_lhs, sf, atom_lhs_coeff );
                        if( !atom_lhs.isNull() ){
                          computeProgVars( atom_lhs );
                          if( !atom_lhs_coeff.isNull() ){
                            atom_rhs = Rewriter::rewrite( NodeManager::currentNM()->mkNode( MULT, atom_lhs_coeff, atom_rhs ) );
                          }
                        }
                      }
                      //if it contains pv, not infinity
                      if( !atom_lhs.isNull() && d_prog_var[atom_lhs].find( pv )!=d_prog_var[atom_lhs].end() ){
                        Node satom = NodeManager::currentNM()->mkNode( atom.getKind(), atom_lhs, atom_rhs );
                        //cannot contain infinity?
                        //if( !d_qe->getTermDatabase()->containsVtsInfinity( atom_lhs ) ){
                        Trace("cbqi-inst-debug") << "..[3] From assertion : " << atom << ", pol = " << pol << std::endl;
                        Trace("cbqi-inst-debug") << "         substituted : " << satom << ", pol = " << pol << std::endl;
                        Node vts_coeff_inf;
                        Node vts_coeff_delta;
                        Node val;
                        Node veq_c;
                        //isolate pv in the inequality
                        int ires = solve_arith( pv, satom, veq_c, val, vts_coeff_inf, vts_coeff_delta );
                        if( ires!=0 ){
                          //disequalities are either strict upper or lower bounds
                          unsigned rmax = ( atom.getKind()==GEQ || options::cbqiModel() ) ? 1 : 2;
                          for( unsigned r=0; r<rmax; r++ ){
                            int uires = ires;
                            Node uval = val;
                            if( atom.getKind()==GEQ ){
                              //push negation downwards
                              if( !pol ){
                                uires = -ires;
                                if( pvtn.isInteger() ){
                                  uval = NodeManager::currentNM()->mkNode( PLUS, val, NodeManager::currentNM()->mkConst( Rational( uires ) ) );
                                  uval = Rewriter::rewrite( uval );
                                }else{
                                  Assert( pvtn.isReal() );
                                  //now is strict inequality
                                  uires = uires*2;
                                }
                              }
                            }else{
                              bool is_upper;
                              if( options::cbqiModel() ){
                                // disequality is a disjunction : only consider the bound in the direction of the model
                                //first check if there is an infinity...
                                if( !vts_coeff_inf.isNull() ){
                                  //coefficient or val won't make a difference, just compare with zero
                                  Trace("cbqi-inst-debug") << "Disequality : check infinity polarity " << vts_coeff_inf << std::endl;
                                  Assert( vts_coeff_inf.isConst() );
                                  is_upper = ( vts_coeff_inf.getConst<Rational>().sgn()==1 );
                                }else{
                                  Node rhs_value = getModelValue( val );
                                  Node lhs_value = pv_value;
                                  if( !veq_c.isNull() ){
                                    lhs_value = NodeManager::currentNM()->mkNode( MULT, lhs_value, veq_c );
                                    lhs_value = Rewriter::rewrite( lhs_value );
                                  }
                                  Trace("cbqi-inst-debug") << "Disequality : check model values " << lhs_value << " " << rhs_value << std::endl;
                                  Assert( lhs_value!=rhs_value );
                                  Node cmp = NodeManager::currentNM()->mkNode( GEQ, lhs_value, rhs_value );
                                  cmp = Rewriter::rewrite( cmp );
                                  Assert( cmp.isConst() );
                                  is_upper = ( cmp!=d_true );
                                }
                              }else{
                                is_upper = (r==0);
                              }
                              Assert( atom.getKind()==EQUAL && !pol );
                              if( pvtn.isInteger() ){
                                uires = is_upper ? -1 : 1;
                                uval = NodeManager::currentNM()->mkNode( PLUS, val, NodeManager::currentNM()->mkConst( Rational( uires ) ) );
                                uval = Rewriter::rewrite( uval );
                              }else{
                                Assert( pvtn.isReal() );
                                uires = is_upper ? -2 : 2;
                              }
                            }
                            Trace("cbqi-bound-inf") << "From " << lit << ", got : ";
                            if( !veq_c.isNull() ){
                              Trace("cbqi-bound-inf") << veq_c << " * ";
                            }
                            Trace("cbqi-bound-inf") << pv << " -> " << uval << ", styp = " << uires << std::endl;
                            //take into account delta
                            if( d_use_vts_delta && ( uires==2 || uires==-2 ) ){
                              if( options::cbqiModel() ){
                                Node delta_coeff = NodeManager::currentNM()->mkConst( Rational( uires > 0 ? 1 : -1 ) );
                                if( vts_coeff_delta.isNull() ){
                                  vts_coeff_delta = delta_coeff;
                                }else{
                                  vts_coeff_delta = NodeManager::currentNM()->mkNode( PLUS, vts_coeff_delta, delta_coeff );
                                  vts_coeff_delta = Rewriter::rewrite( vts_coeff_delta );
                                }
                              }else{
                                Node delta = d_qe->getTermDatabase()->getVtsDelta();
                                uval = NodeManager::currentNM()->mkNode( uires==2 ? PLUS : MINUS, uval, delta );
                                uval = Rewriter::rewrite( uval );
                              }
                            }
                            if( options::cbqiModel() ){
                              //just store bounds, will choose based on tighest bound
                              unsigned index = uires>0 ? 0 : 1;
                              mbp_bounds[index].push_back( uval );
                              mbp_coeff[index].push_back( veq_c );
                              Trace("cbqi-inst-debug") << "Store bound " << index << " " << uval << " " << veq_c << " " << vts_coeff_inf << " " << vts_coeff_delta << " " << lit << std::endl;
                              for( unsigned t=0; t<2; t++ ){
                                mbp_vts_coeff[index][t].push_back( t==0 ? vts_coeff_inf : vts_coeff_delta );
                              }
                              mbp_lit[index].push_back( lit );
                            }else{
                              //try this bound
                              if( doAddInstantiationInc( pv, uval, veq_c, uires>0 ? 1 : -1, sf, effort ) ){
                                return true;
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
        if( vinst->processAssertions( this, sf, pv, lits, effort ) ){
          return true;
        }
        if( options::cbqiModel() ){
          if( pvtn.isInteger() || pvtn.isReal() ){
            bool use_inf = d_use_vts_inf && ( pvtn.isInteger() ? options::cbqiUseInfInt() : options::cbqiUseInfReal() );
            bool upper_first = false;
            if( options::cbqiMinBounds() ){
              upper_first = mbp_bounds[1].size()<mbp_bounds[0].size();
            }
            int best_used[2];
            std::vector< Node > t_values[3];
            //try optimal bounds
            for( unsigned r=0; r<2; r++ ){
              int rr = upper_first ? (1-r) : r;
              best_used[rr] = -1;
              if( mbp_bounds[rr].empty() ){
                if( use_inf ){
                  Trace("cbqi-bound") << "No " << ( rr==0 ? "lower" : "upper" ) << " bounds for " << pv << " (type=" << pvtn << ")" << std::endl;
                  //no bounds, we do +- infinity
                  Node val = d_qe->getTermDatabase()->getVtsInfinity( pvtn );
                  //TODO : rho value for infinity?
                  if( rr==0 ){
                    val = NodeManager::currentNM()->mkNode( UMINUS, val );
                    val = Rewriter::rewrite( val );
                  }
                  if( doAddInstantiationInc( pv, val, Node::null(), 0, sf, effort ) ){
                    return true;
                  }
                }
              }else{
                Trace("cbqi-bound") << ( rr==0 ? "Lower" : "Upper" ) << " bounds for " << pv << " (type=" << pvtn << ") : " << std::endl;
                int best = -1;
                Node best_bound_value[3];
                for( unsigned j=0; j<mbp_bounds[rr].size(); j++ ){
                  Node value[3];
                  if( Trace.isOn("cbqi-bound") ){
                    Assert( !mbp_bounds[rr][j].isNull() );
                    Trace("cbqi-bound") << "  " << j << ": " << mbp_bounds[rr][j];
                    if( !mbp_vts_coeff[rr][0][j].isNull() ){
                      Trace("cbqi-bound") << " (+ " << mbp_vts_coeff[rr][0][j] << " * INF)";
                    }
                    if( !mbp_vts_coeff[rr][1][j].isNull() ){
                      Trace("cbqi-bound") << " (+ " << mbp_vts_coeff[rr][1][j] << " * DELTA)";
                    }
                    if( !mbp_coeff[rr][j].isNull() ){
                      Trace("cbqi-bound") << " (div " << mbp_coeff[rr][j] << ")";
                    }
                    Trace("cbqi-bound") << ", value = ";
                  }
                  t_values[rr].push_back( Node::null() );
                  //check if it is better than the current best bound : lexicographic order infinite/finite/infinitesimal parts
                  bool new_best = true;
                  for( unsigned t=0; t<3; t++ ){
                    //get the value
                    if( t==0 ){
                      value[0] = mbp_vts_coeff[rr][0][j];
                      if( !value[0].isNull() ){
                        Trace("cbqi-bound") << "( " << value[0] << " * INF ) + ";
                      }else{
                        value[0] = d_zero;
                      }
                    }else if( t==1 ){
                      Node t_value = getModelValue( mbp_bounds[rr][j] );
                      t_values[rr][j] = t_value;
                      value[1] = t_value;
                      Trace("cbqi-bound") << value[1];
                    }else{
                      value[2] = mbp_vts_coeff[rr][1][j];
                      if( !value[2].isNull() ){
                        Trace("cbqi-bound") << " + ( " << value[2] << " * DELTA )";
                      }else{
                        value[2] = d_zero;
                      }
                    }
                    //multiply by coefficient
                    if( value[t]!=d_zero && !mbp_coeff[rr][j].isNull() ){
                      Assert( mbp_coeff[rr][j].isConst() );
                      value[t] = NodeManager::currentNM()->mkNode( MULT, NodeManager::currentNM()->mkConst( Rational(1) / mbp_coeff[rr][j].getConst<Rational>() ), value[t] );
                      value[t] = Rewriter::rewrite( value[t] );
                    }
                    //check if new best
                    if( best!=-1 ){
                      Assert( !value[t].isNull() && !best_bound_value[t].isNull() );
                      if( value[t]!=best_bound_value[t] ){
                        Kind k = rr==0 ? GEQ : LEQ;
                        Node cmp_bound = NodeManager::currentNM()->mkNode( k, value[t], best_bound_value[t] );
                        cmp_bound = Rewriter::rewrite( cmp_bound );
                        if( cmp_bound!=d_true ){
                          new_best = false;
                          break;
                        }
                      }
                    }
                  }
                  Trace("cbqi-bound") << std::endl;
                  if( new_best ){
                    for( unsigned t=0; t<3; t++ ){
                      best_bound_value[t] = value[t];
                    }
                    best = j;
                  }
                }
                if( best!=-1 ){
                  Trace("cbqi-bound") << "...best bound is " << best << " : ";
                  if( best_bound_value[0]!=d_zero ){
                    Trace("cbqi-bound") << "( " << best_bound_value[0] << " * INF ) + ";
                  }
                  Trace("cbqi-bound") << best_bound_value[1];
                  if( best_bound_value[2]!=d_zero ){
                    Trace("cbqi-bound") << " + ( " << best_bound_value[2] << " * DELTA )";
                  }
                  Trace("cbqi-bound") << std::endl;
                  best_used[rr] = best;
                  //if using cbqiMidpoint, only add the instance based on one bound if the bound is non-strict
                  if( !options::cbqiMidpoint() || pvtn.isInteger() || mbp_vts_coeff[rr][1][best].isNull() ){
                    Node val = mbp_bounds[rr][best];
                    val = getModelBasedProjectionValue( pv, val, rr==0, mbp_coeff[rr][best], pv_value, t_values[rr][best], sf.d_theta,
                                                        mbp_vts_coeff[rr][0][best], mbp_vts_coeff[rr][1][best] );
                    if( !val.isNull() ){
                      if( doAddInstantiationInc( pv, val, mbp_coeff[rr][best], rr==0 ? 1 : -1, sf, effort ) ){
                        return true;
                      }
                    }
                  }
                }
              }
            }
            //if not using infinity, use model value of zero
            if( !use_inf && mbp_bounds[0].empty() && mbp_bounds[1].empty() ){
              Node val = d_zero;
              Node c; //null (one) coefficient
              val = getModelBasedProjectionValue( pv, val, true, c, pv_value, d_zero, sf.d_theta, Node::null(), Node::null() );
              if( !val.isNull() ){
                if( doAddInstantiationInc( pv, val, c, 0, sf, effort ) ){
                  return true;
                }
              }
            }
            if( options::cbqiMidpoint() && !pvtn.isInteger() ){
              Node vals[2];
              bool bothBounds = true;
              Trace("cbqi-bound") << "Try midpoint of bounds..." << std::endl;
              for( unsigned rr=0; rr<2; rr++ ){
                int best = best_used[rr];
                if( best==-1 ){
                  bothBounds = false;
                }else{
                  vals[rr] = mbp_bounds[rr][best];
                  vals[rr] = getModelBasedProjectionValue( pv, vals[rr], rr==0, Node::null(), pv_value, t_values[rr][best], sf.d_theta,
                                                           mbp_vts_coeff[rr][0][best], Node::null() );
                }
                Trace("cbqi-bound") << "Bound : " << vals[rr] << std::endl;
              }
              Node val;
              if( bothBounds ){
                Assert( !vals[0].isNull() && !vals[1].isNull() );
                if( vals[0]==vals[1] ){
                  val = vals[0];
                }else{
                  val = NodeManager::currentNM()->mkNode( MULT, NodeManager::currentNM()->mkNode( PLUS, vals[0], vals[1] ),
                                                                NodeManager::currentNM()->mkConst( Rational(1)/Rational(2) ) );
                  val = Rewriter::rewrite( val );
                }
              }else{
                if( !vals[0].isNull() ){
                  val = NodeManager::currentNM()->mkNode( PLUS, vals[0], d_one );
                  val = Rewriter::rewrite( val );
                }else if( !vals[1].isNull() ){
                  val = NodeManager::currentNM()->mkNode( MINUS, vals[1], d_one );
                  val = Rewriter::rewrite( val );
                }
              }
              Trace("cbqi-bound") << "Midpoint value : " << val << std::endl;
              if( !val.isNull() ){
                if( doAddInstantiationInc( pv, val, Node::null(), 0, sf, effort ) ){
                  return true;
                }
              }
            }
  #ifdef MBP_STRICT_ASSERTIONS
            Assert( false );
  #endif
            if( options::cbqiNopt() ){
              //try non-optimal bounds (heuristic, may help when nested quantification) ?
              Trace("cbqi-bound") << "Try non-optimal bounds..." << std::endl;
              for( unsigned r=0; r<2; r++ ){
                int rr = upper_first ? (1-r) : r;
                for( unsigned j=0; j<mbp_bounds[rr].size(); j++ ){
                  if( (int)j!=best_used[rr] && ( !options::cbqiMidpoint() || mbp_vts_coeff[rr][1][j].isNull() ) ){
                    Node val = getModelBasedProjectionValue( pv, mbp_bounds[rr][j], rr==0, mbp_coeff[rr][j], pv_value, t_values[rr][j], sf.d_theta,
                                                             mbp_vts_coeff[rr][0][j], mbp_vts_coeff[rr][1][j] );
                    if( !val.isNull() ){
                      if( doAddInstantiationInc( pv, val, mbp_coeff[rr][j], rr==0 ? 1 : -1, sf, effort ) ){
                        return true;
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    //[5] resort to using value in model
    // do so if we are in effort=1, or if the variable is boolean, or if we are solving for a subfield of a datatype
    bool use_model_value = vinst->useModelValue( this, sf, pv, effort );
    if( ( effort>0 || use_model_value || is_cv ) && vinst->allowModelValue( this, sf, pv, effort ) ){

#ifdef CVC4_ASSERTIONS
      if( pvtn.isReal() && options::cbqiNestedQE() && !options::cbqiAll() ){
        Trace("cbqi-warn") << "Had to resort to model value." << std::endl;
        Assert( false );
      }
#endif
      Node mv = getModelValue( pv );
      Node pv_coeff_m;
      Trace("cbqi-inst-debug") << "[5] " << i << "...try model value " << mv << std::endl;
      int new_effort = use_model_value ? effort : 1;
#ifdef MBP_STRICT_ASSERTIONS
      //we only resort to values in the case of booleans
      Assert( ( pvtn.isInteger() ? !options::cbqiUseInfInt() : !options::cbqiUseInfReal() ) || pvtn.isBoolean() );
#endif
      if( doAddInstantiationInc( pv, mv, pv_coeff_m, 0, sf, new_effort ) ){
        return true;
      }
    }
    Trace("cbqi-inst-debug") << "[No instantiation found for " << pv << "]" << std::endl;
    if( is_cv ){  
      d_stack_vars.push_back( pv );
    }
    if( vinst!=NULL ){
      d_active_instantiators.erase( vinst );
    }
    unregisterInstantiationVariable( pv );
    return false;
  }
}

void CegInstantiator::pushStackVariable( Node v ) {
  d_stack_vars.push_back( v );
}

void CegInstantiator::popStackVariable() {
  Assert( !d_stack_vars.empty() );
  d_stack_vars.pop_back();
}

bool CegInstantiator::doAddInstantiationInc( Node pv, Node n, Node pv_coeff, int bt, SolvedForm& sf, unsigned effort ) {
  if( d_curr_subs_proc[pv][n].find( pv_coeff )==d_curr_subs_proc[pv][n].end() ){
    d_curr_subs_proc[pv][n][pv_coeff] = true;
    if( Trace.isOn("cbqi-inst") ){
      for( unsigned j=0; j<sf.d_subs.size(); j++ ){
        Trace("cbqi-inst") << " ";
      }
      Trace("cbqi-inst") << sf.d_subs.size() << ": ";
      if( !pv_coeff.isNull() ){
        Trace("cbqi-inst") << pv_coeff << " * ";
      }
      Trace("cbqi-inst") << pv << " -> " << n << std::endl;
      Assert( n.getType().isSubtypeOf( pv.getType() ) );
    }
    //must ensure variables have been computed for n
    computeProgVars( n );
    Assert( d_inelig.find( n )==d_inelig.end() );

    //substitute into previous substitutions, when applicable
    std::vector< Node > a_subs;
    a_subs.push_back( n );
    std::vector< Node > a_var;
    a_var.push_back( pv );
    std::vector< Node > a_coeff;
    std::vector< Node > a_has_coeff;
    if( !pv_coeff.isNull() ){
      a_coeff.push_back( pv_coeff );
      a_has_coeff.push_back( pv );
    }
    bool success = true;
    std::map< int, Node > prev_subs;
    std::map< int, Node > prev_coeff;
    std::map< int, Node > prev_sym_subs;
    std::vector< Node > new_has_coeff;
    Trace("cbqi-inst-debug2") << "Applying substitutions..." << std::endl;
    for( unsigned j=0; j<sf.d_subs.size(); j++ ){
      Trace("cbqi-inst-debug2") << "  Apply for " << sf.d_subs[j]  << std::endl;
      Assert( d_prog_var.find( sf.d_subs[j] )!=d_prog_var.end() );
      if( d_prog_var[sf.d_subs[j]].find( pv )!=d_prog_var[sf.d_subs[j]].end() ){
        prev_subs[j] = sf.d_subs[j];
        TNode tv = pv;
        TNode ts = n;
        Node a_pv_coeff;
        Node new_subs = applySubstitution( sf.d_vars[j].getType(), sf.d_subs[j], a_subs, a_coeff, a_has_coeff, a_var, a_pv_coeff, true );
        if( !new_subs.isNull() ){
          sf.d_subs[j] = new_subs;
          if( !a_pv_coeff.isNull() ){
            prev_coeff[j] = sf.d_coeff[j];
            if( sf.d_coeff[j].isNull() ){
              Assert( std::find( sf.d_has_coeff.begin(), sf.d_has_coeff.end(), sf.d_vars[j] )==sf.d_has_coeff.end() );
              //now has coefficient
              new_has_coeff.push_back( sf.d_vars[j] );
              sf.d_has_coeff.push_back( sf.d_vars[j] );
              sf.d_coeff[j] = a_pv_coeff;
            }else{
              sf.d_coeff[j] = Rewriter::rewrite( NodeManager::currentNM()->mkNode( MULT, sf.d_coeff[j], a_pv_coeff ) );
            }
          }
          if( sf.d_subs[j]!=prev_subs[j] ){
            computeProgVars( sf.d_subs[j] );
            Assert( d_inelig.find( sf.d_subs[j] )==d_inelig.end() );
          }
          Trace("cbqi-inst-debug2") << "Subs " << j << " " << sf.d_subs[j] << std::endl;
        }else{
          Trace("cbqi-inst-debug2") << "...failed to apply substitution to " << sf.d_subs[j] << std::endl;
          success = false;
          break;
        }
      }else{
        Trace("cbqi-inst-debug2") << "Skip " << j << " " << sf.d_subs[j] << std::endl;
      }
    }
    if( success ){
      Trace("cbqi-inst-debug2") << "Adding to vectors..." << std::endl;
      sf.push_back( pv, n, pv_coeff, bt );
      Node prev_theta = sf.d_theta;
      Node new_theta = sf.d_theta;
      if( !pv_coeff.isNull() ){
        if( new_theta.isNull() ){
          new_theta = pv_coeff;
        }else{
          new_theta = NodeManager::currentNM()->mkNode( MULT, new_theta, pv_coeff );
          new_theta = Rewriter::rewrite( new_theta );
        }
      }
      sf.d_theta = new_theta;
      Trace("cbqi-inst-debug2") << "Recurse..." << std::endl;
      unsigned i = d_curr_index[pv];
      success = doAddInstantiation( sf, d_stack_vars.empty() ? i+1 : i, effort );
      sf.d_theta = prev_theta;
      if( !success ){
        Trace("cbqi-inst-debug2") << "Removing from vectors..." << std::endl;
        sf.pop_back( pv, n, pv_coeff, bt );
      }
    }
    if( success ){
      return true;
    }else{
      Trace("cbqi-inst-debug2") << "Revert substitutions..." << std::endl;
      //revert substitution information
      for( std::map< int, Node >::iterator it = prev_subs.begin(); it != prev_subs.end(); it++ ){
        sf.d_subs[it->first] = it->second;
      }
      for( std::map< int, Node >::iterator it = prev_coeff.begin(); it != prev_coeff.end(); it++ ){
        sf.d_coeff[it->first] = it->second;
      }
      for( unsigned i=0; i<new_has_coeff.size(); i++ ){
        sf.d_has_coeff.pop_back();
      }
      return false;
    }
  }else{
    //already tried this substitution
    return false;
  }
}

bool CegInstantiator::processInstantiationCoeff( SolvedForm& sf ) {
  for( unsigned j=0; j<sf.d_has_coeff.size(); j++ ){
    Assert( std::find( sf.d_vars.begin(), sf.d_vars.end(), sf.d_has_coeff[j] )!=sf.d_vars.end() );
    unsigned index = std::find( sf.d_vars.begin(), sf.d_vars.end(), sf.d_has_coeff[j] )-sf.d_vars.begin();
    Assert( !sf.d_coeff[index].isNull() );
    Trace("cbqi-inst-debug") << "Normalize substitution for " << sf.d_coeff[index] << " * " << sf.d_vars[index] << " = " << sf.d_subs[index] << std::endl;
    Assert( sf.d_vars[index].getType().isInteger() );
    //must ensure that divisibility constraints are met
    //solve updated rewritten equality for vars[index], if coefficient is one, then we are successful
    Node eq_lhs = NodeManager::currentNM()->mkNode( MULT, sf.d_coeff[index], sf.d_vars[index] );
    Node eq_rhs = sf.d_subs[index];
    Node eq = eq_lhs.eqNode( eq_rhs );
    eq = Rewriter::rewrite( eq );
    Trace("cbqi-inst-debug") << "...equality is " << eq << std::endl;
    std::map< Node, Node > msum;
    if( QuantArith::getMonomialSumLit( eq, msum ) ){
      Node veq;
      if( QuantArith::isolate( sf.d_vars[index], msum, veq, EQUAL, true )!=0 ){
        Node veq_c;
        if( veq[0]!=sf.d_vars[index] ){
          Node veq_v;
          if( QuantArith::getMonomial( veq[0], veq_c, veq_v ) ){
            Assert( veq_v==sf.d_vars[index] );
          }
        }
        sf.d_subs[index] = veq[1];
        if( !veq_c.isNull() ){
          sf.d_subs[index] = NodeManager::currentNM()->mkNode( INTS_DIVISION_TOTAL, veq[1], veq_c );
          Trace("cbqi-inst-debug") << "...bound type is : " << sf.d_btyp[index] << std::endl;
          //intger division rounding up if from a lower bound
          if( sf.d_btyp[index]==1 && options::cbqiRoundUpLowerLia() ){
            sf.d_subs[index] = NodeManager::currentNM()->mkNode( PLUS, sf.d_subs[index],
              NodeManager::currentNM()->mkNode( ITE,
                NodeManager::currentNM()->mkNode( EQUAL,
                  NodeManager::currentNM()->mkNode( INTS_MODULUS_TOTAL, veq[1], veq_c ),
                  d_zero ),
                d_zero, d_one )
            );
          }
        }
        Trace("cbqi-inst-debug") << "...normalize integers : " << sf.d_vars[index] << " -> " << sf.d_subs[index] << std::endl;
      }else{
        Trace("cbqi-inst-debug") << "...failed." << std::endl;
        return false;
      }
    }else{
      Trace("cbqi-inst-debug") << "...failed." << std::endl;
      return false;
    }
  }
  return true;
}

bool CegInstantiator::doAddInstantiation( std::vector< Node >& subs, std::vector< Node >& vars ) {
  if( vars.size()>d_vars.size() ){
    Trace("cbqi-inst-debug") << "Reconstructing instantiations...." << std::endl;
    std::map< Node, Node > subs_map;
    for( unsigned i=0; i<subs.size(); i++ ){
      subs_map[vars[i]] = subs[i];
    }
    subs.clear();
    for( unsigned i=0; i<d_vars.size(); i++ ){
      std::map< Node, Node >::iterator it = subs_map.find( d_vars[i] );
      Assert( it!=subs_map.end() );
      Node n = it->second;
      Trace("cbqi-inst-debug") << "  " << d_vars[i] << " -> " << n << std::endl;
      subs.push_back( n );
    }
  }
  if( !d_var_order_index.empty() ){
    std::vector< Node > subs_orig;
    subs_orig.insert( subs_orig.end(), subs.begin(), subs.end() );
    subs.clear();
    for( unsigned i=0; i<subs_orig.size(); i++ ){
      subs.push_back( subs_orig[d_var_order_index[i]] );
    }
  }
  bool ret = d_out->doAddInstantiation( subs );
#ifdef MBP_STRICT_ASSERTIONS
  Assert( ret );
#endif
  return ret;
}

Node CegInstantiator::applySubstitution( TypeNode tn, Node n, std::vector< Node >& subs, std::vector< Node >& coeff, std::vector< Node >& has_coeff,
                                         std::vector< Node >& vars, Node& pv_coeff, bool try_coeff ) {
  Assert( d_prog_var.find( n )!=d_prog_var.end() );
  Assert( n==Rewriter::rewrite( n ) );
  bool req_coeff = false;
  if( !has_coeff.empty() ){
    for( std::map< Node, bool >::iterator it = d_prog_var[n].begin(); it != d_prog_var[n].end(); ++it ){
      if( std::find( has_coeff.begin(), has_coeff.end(), it->first )!=has_coeff.end() ){
        req_coeff = true;
        break;
      }
    }
  }
  if( !req_coeff ){
    Node nret = n.substitute( vars.begin(), vars.end(), subs.begin(), subs.end() );
    if( n!=nret ){
      nret = Rewriter::rewrite( nret );
    }
    return nret;
  }else{
    if( !tn.isInteger() ){
      //can do basic substitution instead with divisions
      std::vector< Node > nvars;
      std::vector< Node > nsubs;
      for( unsigned i=0; i<vars.size(); i++ ){
        if( !coeff[i].isNull() ){
          Assert( coeff[i].isConst() );
          nsubs.push_back( Rewriter::rewrite( NodeManager::currentNM()->mkNode( MULT, subs[i], NodeManager::currentNM()->mkConst( Rational(1)/coeff[i].getConst<Rational>() ) ) ));
        }else{
          nsubs.push_back( subs[i] );
        }
      }
      Node nret = n.substitute( vars.begin(), vars.end(), nsubs.begin(), nsubs.end() );
      if( n!=nret ){
        nret = Rewriter::rewrite( nret );
      }
      return nret;
    }else if( try_coeff ){
      //must convert to monomial representation
      std::map< Node, Node > msum;
      if( QuantArith::getMonomialSum( n, msum ) ){
        std::map< Node, Node > msum_coeff;
        std::map< Node, Node > msum_term;
        for( std::map< Node, Node >::iterator it = msum.begin(); it != msum.end(); ++it ){
          //check if in substitution
          std::vector< Node >::iterator its = std::find( vars.begin(), vars.end(), it->first );
          if( its!=vars.end() ){
            int index = its-vars.begin();
            if( coeff[index].isNull() ){
              //apply substitution
              msum_term[it->first] = subs[index];
            }else{
              //apply substitution, multiply to ensure no divisibility conflict
              msum_term[it->first] = subs[index];
              //relative coefficient
              msum_coeff[it->first] = coeff[index];
              if( pv_coeff.isNull() ){
                pv_coeff = coeff[index];
              }else{
                pv_coeff = NodeManager::currentNM()->mkNode( MULT, pv_coeff, coeff[index] );
              }
            }
          }else{
            msum_term[it->first] = it->first;
          }
        }
        //make sum with normalized coefficient
        Assert( !pv_coeff.isNull() );
        pv_coeff = Rewriter::rewrite( pv_coeff );
        Trace("cegqi-si-apply-subs-debug") << "Combined coeff : " << pv_coeff << std::endl;
        std::vector< Node > children;
        for( std::map< Node, Node >::iterator it = msum.begin(); it != msum.end(); ++it ){
          Node c_coeff;
          if( !msum_coeff[it->first].isNull() ){
            c_coeff = Rewriter::rewrite( NodeManager::currentNM()->mkConst( pv_coeff.getConst<Rational>() / msum_coeff[it->first].getConst<Rational>() ) );
          }else{
            c_coeff = pv_coeff;
          }
          if( !it->second.isNull() ){
            c_coeff = NodeManager::currentNM()->mkNode( MULT, c_coeff, it->second );
          }
          Assert( !c_coeff.isNull() );
          Node c;
          if( msum_term[it->first].isNull() ){
            c = c_coeff;
          }else{
            c = NodeManager::currentNM()->mkNode( MULT, c_coeff, msum_term[it->first] );
          }
          children.push_back( c );
          Trace("cegqi-si-apply-subs-debug") << "Add child : " << c << std::endl;
        }
        Node nret = children.size()==1 ? children[0] : NodeManager::currentNM()->mkNode( PLUS, children );
        nret = Rewriter::rewrite( nret );
        //result is ( nret / pv_coeff )
        return nret;
      }else{
        Trace("cegqi-si-apply-subs-debug") << "Failed to find monomial sum " << n << std::endl;
      }
    }
    // failed to apply the substitution
    return Node::null();
  }
}

Node CegInstantiator::getModelBasedProjectionValue( Node e, Node t, bool isLower, Node c, Node me, Node mt, Node theta, Node inf_coeff, Node delta_coeff ) {
  Node val = t;
  Trace("cbqi-bound2") << "Value : " << val << std::endl;
  Assert( !e.getType().isInteger() || t.getType().isInteger() );
  Assert( !e.getType().isInteger() || mt.getType().isInteger() );
  //add rho value
  //get the value of c*e
  Node ceValue = me;
  Node new_theta = theta;
  if( !c.isNull() ){
    Assert( c.getType().isInteger() );
    ceValue = NodeManager::currentNM()->mkNode( MULT, ceValue, c );
    ceValue = Rewriter::rewrite( ceValue );
    if( new_theta.isNull() ){
      new_theta = c;
    }else{
      new_theta = NodeManager::currentNM()->mkNode( MULT, new_theta, c );
      new_theta = Rewriter::rewrite( new_theta );
    }
    Trace("cbqi-bound2") << "...c*e = " << ceValue << std::endl;
    Trace("cbqi-bound2") << "...theta = " << new_theta << std::endl;
  }
  if( !new_theta.isNull() && e.getType().isInteger() ){
    Node rho;
    //if( !mt.getType().isInteger() ){
      //round up/down
      //mt = NodeManager::currentNM()->mkNode(
    //}
    if( isLower ){
      rho = NodeManager::currentNM()->mkNode( MINUS, ceValue, mt );
    }else{
      rho = NodeManager::currentNM()->mkNode( MINUS, mt, ceValue );
    }
    rho = Rewriter::rewrite( rho );
    Trace("cbqi-bound2") << "...rho = " << me << " - " << mt << " = " << rho << std::endl;
    Trace("cbqi-bound2") << "..." << rho << " mod " << new_theta << " = ";
    rho = NodeManager::currentNM()->mkNode( INTS_MODULUS_TOTAL, rho, new_theta );
    rho = Rewriter::rewrite( rho );
    Trace("cbqi-bound2") << rho << std::endl;
    Kind rk = isLower ? PLUS : MINUS;
    val = NodeManager::currentNM()->mkNode( rk, val, rho );
    val = Rewriter::rewrite( val );
    Trace("cbqi-bound2") << "(after rho) : " << val << std::endl;
  }
  if( !inf_coeff.isNull() ){
    Assert( !d_vts_sym[0].isNull() );
    val = NodeManager::currentNM()->mkNode( PLUS, val, NodeManager::currentNM()->mkNode( MULT, inf_coeff, d_vts_sym[0] ) );
    val = Rewriter::rewrite( val );
  }
  if( !delta_coeff.isNull() ){
    //create delta here if necessary
    if( d_vts_sym[1].isNull() ){
      d_vts_sym[1] = d_qe->getTermDatabase()->getVtsDelta();
    }
    val = NodeManager::currentNM()->mkNode( PLUS, val, NodeManager::currentNM()->mkNode( MULT, delta_coeff, d_vts_sym[1] ) );
    val = Rewriter::rewrite( val );
  }
  return val;
}

bool CegInstantiator::check() {
  if( d_qe->getTheoryEngine()->needCheck() ){
    Trace("cbqi-engine") << "  CEGQI instantiator : wait until all ground theories are finished." << std::endl;
    return false;
  }
  processAssertions();
  for( unsigned r=0; r<2; r++ ){
    SolvedForm sf;
    d_stack_vars.clear();
    //try to add an instantiation
    if( doAddInstantiation( sf, 0, r==0 ? 0 : 2 ) ){
      return true;
    }
  }
  Trace("cbqi-engine") << "  WARNING : unable to find CEGQI single invocation instantiation." << std::endl;
  return false;
}

void collectPresolveEqTerms( Node n, std::map< Node, std::vector< Node > >& teq ) {
  if( n.getKind()==FORALL || n.getKind()==EXISTS ){
    //do nothing
  }else{
    if( n.getKind()==EQUAL ){
      for( unsigned i=0; i<2; i++ ){
        std::map< Node, std::vector< Node > >::iterator it = teq.find( n[i] );
        if( it!=teq.end() ){
          Node nn = n[ i==0 ? 1 : 0 ];
          if( std::find( it->second.begin(), it->second.end(), nn )==it->second.end() ){
            it->second.push_back( nn );
            Trace("cbqi-presolve") << "  - " << n[i] << " = " << nn << std::endl;
          }
        }
      }
    }
    for( unsigned i=0; i<n.getNumChildren(); i++ ){
      collectPresolveEqTerms( n[i], teq );
    }
  }
}

void getPresolveEqConjuncts( std::vector< Node >& vars, std::vector< Node >& terms,
                             std::map< Node, std::vector< Node > >& teq, Node f, std::vector< Node >& conj ) {
  if( conj.size()<1000 ){
    if( terms.size()==f[0].getNumChildren() ){
      Node c = f[1].substitute( vars.begin(), vars.end(), terms.begin(), terms.end() );
      conj.push_back( c );
    }else{
      unsigned i = terms.size();
      Node v = f[0][i];
      terms.push_back( Node::null() );
      for( unsigned j=0; j<teq[v].size(); j++ ){
        terms[i] = teq[v][j];
        getPresolveEqConjuncts( vars, terms, teq, f, conj );
      }
      terms.pop_back();
    }
  }
}

void CegInstantiator::presolve( Node q ) {
  //at preregister time, add proxy of obvious instantiations up front, which helps learning during preprocessing
  //only if no nested quantifiers
  if( !QuantifiersRewriter::containsQuantifiers( q[1] ) ){
    std::vector< Node > ps_vars;
    std::map< Node, std::vector< Node > > teq;
    for( unsigned i=0; i<q[0].getNumChildren(); i++ ){
      ps_vars.push_back( q[0][i] );
      teq[q[0][i]].clear();
    }
    collectPresolveEqTerms( q[1], teq );
    std::vector< Node > terms;
    std::vector< Node > conj;
    getPresolveEqConjuncts( ps_vars, terms, teq, q, conj );

    if( !conj.empty() ){
      Node lem = conj.size()==1 ? conj[0] : NodeManager::currentNM()->mkNode( AND, conj );
      Node g = NodeManager::currentNM()->mkSkolem( "g", NodeManager::currentNM()->booleanType() );
      lem = NodeManager::currentNM()->mkNode( OR, g, lem );
      Trace("cbqi-presolve-debug") << "Presolve lemma : " << lem << std::endl;
      d_qe->getOutputChannel().lemma( lem, false, true );
    }
  }
}

void collectTheoryIds( TypeNode tn, std::map< TypeNode, bool >& visited, std::vector< TheoryId >& tids ){
  std::map< TypeNode, bool >::iterator itt = visited.find( tn );
  if( itt==visited.end() ){
    visited[tn] = true;
    TheoryId tid = Theory::theoryOf( tn );
    if( std::find( tids.begin(), tids.end(), tid )==tids.end() ){
      tids.push_back( tid );
    }
    if( tn.isDatatype() ){
      const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
      for( unsigned i=0; i<dt.getNumConstructors(); i++ ){
        for( unsigned j=0; j<dt[i].getNumArgs(); j++ ){
          collectTheoryIds( TypeNode::fromType( ((SelectorType)dt[i][j].getType()).getRangeType() ), visited, tids );
        }
      }
    }
  }
}

void CegInstantiator::processAssertions() {
  Trace("cbqi-proc") << "--- Process assertions, #var = " << d_vars.size() << ", #aux-var = " << d_aux_vars.size() << std::endl;
  d_curr_asserts.clear();
  d_curr_eqc.clear();
  d_curr_type_eqc.clear();

  eq::EqualityEngine* ee = d_qe->getMasterEqualityEngine();
  //to eliminate identified illegal terms
  std::map< Node, Node > aux_subs;

  //for each variable
  std::vector< TheoryId > tids;
  tids.push_back(THEORY_UF);
  for( unsigned i=0; i<d_vars.size(); i++ ){
    Node pv = d_vars[i];
    TypeNode pvtn = pv.getType();
    //collect relevant theories
    std::map< TypeNode, bool > visited;
    collectTheoryIds( pvtn, visited, tids );
    //collect information about eqc
    if( ee->hasTerm( pv ) ){
      Node pvr = ee->getRepresentative( pv );
      if( d_curr_eqc.find( pvr )==d_curr_eqc.end() ){
        Trace("cbqi-proc") << "Collect equivalence class " << pvr << std::endl;
        eq::EqClassIterator eqc_i = eq::EqClassIterator( pvr, ee );
        while( !eqc_i.isFinished() ){
          d_curr_eqc[pvr].push_back( *eqc_i );
          ++eqc_i;
        }
      }
    }
  }
  //collect assertions for relevant theories
  for( unsigned i=0; i<tids.size(); i++ ){
    TheoryId tid = tids[i];
    Theory* theory = d_qe->getTheoryEngine()->theoryOf( tid );
    if( theory && d_qe->getTheoryEngine()->isTheoryEnabled(tid) ){
      Trace("cbqi-proc") << "Collect assertions from theory " << tid << std::endl;
      d_curr_asserts[tid].clear();
      //collect all assertions from theory
      for( context::CDList<Assertion>::const_iterator it = theory->facts_begin(); it != theory->facts_end(); ++ it) {
        Node lit = (*it).assertion;
        Node atom = lit.getKind()==NOT ? lit[0] : lit;
        if( d_is_nested_quant || std::find( d_ce_atoms.begin(), d_ce_atoms.end(), atom )!=d_ce_atoms.end() ){
          d_curr_asserts[tid].push_back( lit );
          Trace("cbqi-proc-debug") << "...add : " << lit << std::endl;
        }else{
          Trace("cbqi-proc") << "...do not consider literal " << tid << " : " << lit << " since it is not part of CE body." << std::endl;
        }
        if( lit.getKind()==EQUAL ){
          std::map< Node, std::map< Node, Node > >::iterator itae = d_aux_eq.find( lit );
          if( itae!=d_aux_eq.end() ){
            for( std::map< Node, Node >::iterator itae2 = itae->second.begin(); itae2 != itae->second.end(); ++itae2 ){
              aux_subs[ itae2->first ] = itae2->second;
              Trace("cbqi-proc") << "......add substitution : " << itae2->first << " -> " << itae2->second << std::endl;
            }
          }
        }
      }
    }
  }
  //collect equivalence classes that correspond to relevant theories
  Trace("cbqi-proc-debug") << "...collect typed equivalence classes" << std::endl;
  eq::EqClassesIterator eqcs_i = eq::EqClassesIterator( ee );
  while( !eqcs_i.isFinished() ){
    Node r = *eqcs_i;
    TypeNode rtn = r.getType();
    TheoryId tid = Theory::theoryOf( rtn );
    //if we care about the theory of this eqc
    if( std::find( tids.begin(), tids.end(), tid )!=tids.end() ){
      if( rtn.isInteger() || rtn.isReal() ){
        rtn = rtn.getBaseType();
      }
      Trace("cbqi-proc-debug") << "...type eqc: " << r << std::endl;
      d_curr_type_eqc[rtn].push_back( r );
      if( d_curr_eqc.find( r )==d_curr_eqc.end() ){
        Trace("cbqi-proc") << "Collect equivalence class " << r << std::endl;
        Trace("cbqi-proc-debug") << "  ";
        eq::EqClassIterator eqc_i = eq::EqClassIterator( r, ee );
        while( !eqc_i.isFinished() ){
          Trace("cbqi-proc-debug") << *eqc_i << " ";
          d_curr_eqc[r].push_back( *eqc_i );
          ++eqc_i;
        }
        Trace("cbqi-proc-debug") << std::endl;
      }
    }
    ++eqcs_i;
  }
  //construct substitution from auxiliary variable equalities (if e.g. ITE removal was applied to CE body of quantified formula)
  std::vector< Node > subs_lhs;
  std::vector< Node > subs_rhs;
  for( unsigned i=0; i<d_aux_vars.size(); i++ ){
    Node r = d_aux_vars[i];
    std::map< Node, Node >::iterator it = aux_subs.find( r );
    if( it!=aux_subs.end() ){
      addToAuxVarSubstitution( subs_lhs, subs_rhs, r, it->second );
    }else{
      Trace("cbqi-proc") << "....no substitution found for auxiliary variable " << r << "!!!" << std::endl;
#ifdef MBP_STRICT_ASSERTIONS
      Assert( false );
#endif
    }
  }

  //apply substitutions to everything, if necessary
  if( !subs_lhs.empty() ){
    Trace("cbqi-proc") << "Applying substitution : " << std::endl;
    for( unsigned i=0; i<subs_lhs.size(); i++ ){
      Trace("cbqi-proc") << "  " << subs_lhs[i] << " -> " << subs_rhs[i] << std::endl;
    }
    for( std::map< TheoryId, std::vector< Node > >::iterator it = d_curr_asserts.begin(); it != d_curr_asserts.end(); ++it ){
      for( unsigned i=0; i<it->second.size(); i++ ){
        Node lit = it->second[i];
        lit = lit.substitute( subs_lhs.begin(), subs_lhs.end(), subs_rhs.begin(), subs_rhs.end() );
        lit = Rewriter::rewrite( lit );
        it->second[i] = lit;
      }
    }
    for( std::map< Node, std::vector< Node > >::iterator it = d_curr_eqc.begin(); it != d_curr_eqc.end(); ++it ){
      for( unsigned i=0; i<it->second.size(); i++ ){
        Node n = it->second[i];
        n = n.substitute( subs_lhs.begin(), subs_lhs.end(), subs_rhs.begin(), subs_rhs.end() );
        n = Rewriter::rewrite( n  );
        it->second[i] = n;
      }
    }
  }

  //remove unecessary assertions
  for( std::map< TheoryId, std::vector< Node > >::iterator it = d_curr_asserts.begin(); it != d_curr_asserts.end(); ++it ){
    std::vector< Node > akeep;
    for( unsigned i=0; i<it->second.size(); i++ ){
      Node n = it->second[i];
      //must be an eligible term
      if( isEligible( n ) ){
        //must contain at least one variable
        if( !d_prog_var[n].empty() ){
          Trace("cbqi-proc") << "...literal[" << it->first << "] : " << n << std::endl;
          akeep.push_back( n );
        }else{
          Trace("cbqi-proc") << "...remove literal from " << it->first << " : " << n << " since it contains no relevant variables." << std::endl;
        }
      }else{
        Trace("cbqi-proc") << "...remove literal from " << it->first << " : " << n << " since it contains ineligible terms." << std::endl;
      }
    }
    it->second.clear();
    it->second.insert( it->second.end(), akeep.begin(), akeep.end() );
  }

  //remove duplicate terms from eqc
  for( std::map< Node, std::vector< Node > >::iterator it = d_curr_eqc.begin(); it != d_curr_eqc.end(); ++it ){
    std::vector< Node > new_eqc;
    for( unsigned i=0; i<it->second.size(); i++ ){
      if( std::find( new_eqc.begin(), new_eqc.end(), it->second[i] )==new_eqc.end() ){
        new_eqc.push_back( it->second[i] );
      }
    }
    it->second.clear();
    it->second.insert( it->second.end(), new_eqc.begin(), new_eqc.end() );
  }
}

void CegInstantiator::addToAuxVarSubstitution( std::vector< Node >& subs_lhs, std::vector< Node >& subs_rhs, Node l, Node r ) {
  r = r.substitute( subs_lhs.begin(), subs_lhs.end(), subs_rhs.begin(), subs_rhs.end() );

  std::vector< Node > cl;
  cl.push_back( l );
  std::vector< Node > cr;
  cr.push_back( r );
  for( unsigned i=0; i<subs_lhs.size(); i++ ){
    Node nr = subs_rhs[i].substitute( cl.begin(), cl.end(), cr.begin(), cr.end() );
    nr = Rewriter::rewrite( nr );
    subs_rhs[i] = nr;
  }

  subs_lhs.push_back( l );
  subs_rhs.push_back( r );
}

Node CegInstantiator::getModelValue( Node n ) {
  return d_qe->getModel()->getValue( n );
}

void CegInstantiator::collectCeAtoms( Node n, std::map< Node, bool >& visited ) {
  if( n.getKind()==FORALL ){
    d_is_nested_quant = true;
  }else if( visited.find( n )==visited.end() ){
    visited[n] = true;
    if( TermDb::isBoolConnective( n.getKind() ) ){
      for( unsigned i=0; i<n.getNumChildren(); i++ ){
        collectCeAtoms( n[i], visited );
      }
    }else{
      if( std::find( d_ce_atoms.begin(), d_ce_atoms.end(), n )==d_ce_atoms.end() ){
        Trace("cbqi-ce-atoms") << "CE atoms : " << n << std::endl;
        d_ce_atoms.push_back( n );
      }
    }
  }
}

struct sortCegVarOrder {
  bool operator() (Node i, Node j) {
    TypeNode it = i.getType();
    TypeNode jt = j.getType();
    return ( it!=jt && jt.isSubtypeOf( it ) ) || ( it==jt && i<j );
  }
};


void CegInstantiator::registerCounterexampleLemma( std::vector< Node >& lems, std::vector< Node >& ce_vars ) {
  //Assert( d_vars.empty() );
  d_vars.clear();
  d_vars.insert( d_vars.end(), ce_vars.begin(), ce_vars.end() );

  //determine variable order: must do Reals before Ints
  if( !d_vars.empty() ){
    TypeNode tn0 = d_vars[0].getType();
    bool doSort = false;
    std::map< Node, unsigned > voo;
    for( unsigned i=0; i<d_vars.size(); i++ ){
      voo[d_vars[i]] = i;
      d_var_order_index.push_back( 0 );
      if( d_vars[i].getType()!=tn0 ){
        doSort = true;
      }
    }
    if( doSort ){
      Trace("cbqi-debug") << "Sort variables based on ordering." << std::endl;
      sortCegVarOrder scvo;
      std::sort( d_vars.begin(), d_vars.end(), scvo );
      Trace("cbqi-debug") << "Consider variables in this order : " << std::endl;
      for( unsigned i=0; i<d_vars.size(); i++ ){
        d_var_order_index[voo[d_vars[i]]] = i;
        Trace("cbqi-debug") << "  " << d_vars[i] << " : " << d_vars[i].getType() << ", index was : " << voo[d_vars[i]] << std::endl;
      }
      Trace("cbqi-debug") << std::endl;
    }else{
      d_var_order_index.clear();
    }
  }

  //remove ITEs
  IteSkolemMap iteSkolemMap;
  d_qe->getTheoryEngine()->getIteRemover()->run(lems, iteSkolemMap);
  //Assert( d_aux_vars.empty() );
  d_aux_vars.clear();
  d_aux_eq.clear();
  for(IteSkolemMap::iterator i = iteSkolemMap.begin(); i != iteSkolemMap.end(); ++i) {
    Trace("cbqi-debug") << "  Auxiliary var (from ITE) : " << i->first << std::endl;
    d_aux_vars.push_back( i->first );
  }
  for( unsigned i=0; i<lems.size(); i++ ){
    Trace("cbqi-debug") << "Counterexample lemma (pre-rewrite)  " << i << " : " << lems[i] << std::endl;
    Node rlem = lems[i];
    rlem = Rewriter::rewrite( rlem );
    Trace("cbqi-debug") << "Counterexample lemma (post-rewrite) " << i << " : " << rlem << std::endl;
    //record the literals that imply auxiliary variables to be equal to terms
    if( lems[i].getKind()==ITE && rlem.getKind()==ITE ){
      if( lems[i][1].getKind()==EQUAL && lems[i][2].getKind()==EQUAL && lems[i][1][0]==lems[i][2][0] ){
        if( std::find( d_aux_vars.begin(), d_aux_vars.end(), lems[i][1][0] )!=d_aux_vars.end() ){
          Node v = lems[i][1][0];
          for( unsigned r=1; r<=2; r++ ){
            d_aux_eq[rlem[r]][v] = lems[i][r][1];
            Trace("cbqi-debug") << "  " << rlem[r] << " implies " << v << " = " << lems[i][r][1] << std::endl;
          }
        }
      }
    }
    lems[i] = rlem;
  }
  //collect atoms from all lemmas: we will only do bounds coming from original body
  d_is_nested_quant = false;
  std::map< Node, bool > visited;
  for( unsigned i=0; i<lems.size(); i++ ){
    collectCeAtoms( lems[i], visited );
  }
}

//this isolates the atom into solved form
//     veq_c * pv <> val + vts_coeff_delta * delta + vts_coeff_inf * inf
//  ensures val is Int if pv is Int, and val does not contain vts symbols
int CegInstantiator::solve_arith( Node pv, Node atom, Node& veq_c, Node& val, Node& vts_coeff_inf, Node& vts_coeff_delta ) {
  int ires = 0;
  Trace("cbqi-inst-debug") << "isolate for " << pv << " in " << atom << std::endl;
  std::map< Node, Node > msum;
  if( QuantArith::getMonomialSumLit( atom, msum ) ){
    Trace("cbqi-inst-debug") << "got monomial sum: " << std::endl;
    if( Trace.isOn("cbqi-inst-debug") ){
      QuantArith::debugPrintMonomialSum( msum, "cbqi-inst-debug" );
    }
    TypeNode pvtn = pv.getType();
    //remove vts symbols from polynomial
    Node vts_coeff[2];
    for( unsigned t=0; t<2; t++ ){
      if( !d_vts_sym[t].isNull() ){
        std::map< Node, Node >::iterator itminf = msum.find( d_vts_sym[t] );
        if( itminf!=msum.end() ){
          vts_coeff[t] = itminf->second;
          if( vts_coeff[t].isNull() ){
            vts_coeff[t] = NodeManager::currentNM()->mkConst( Rational( 1 ) );
          }
          //negate if coefficient on variable is positive
          std::map< Node, Node >::iterator itv = msum.find( pv );
          if( itv!=msum.end() ){
            //multiply by the coefficient we will isolate for
            if( itv->second.isNull() ){
              vts_coeff[t] = QuantArith::negate(vts_coeff[t]);
            }else{
              if( !pvtn.isInteger() ){
                vts_coeff[t] = NodeManager::currentNM()->mkNode( MULT, NodeManager::currentNM()->mkConst( Rational(-1) / itv->second.getConst<Rational>() ), vts_coeff[t] );
                vts_coeff[t] = Rewriter::rewrite( vts_coeff[t] );
              }else if( itv->second.getConst<Rational>().sgn()==1 ){
                vts_coeff[t] = QuantArith::negate(vts_coeff[t]);
              }
            }
          }
          Trace("cbqi-inst-debug") << "vts[" << t << "] coefficient is " << vts_coeff[t] << std::endl;
          msum.erase( d_vts_sym[t] );
        }
      }
    }

    ires = QuantArith::isolate( pv, msum, veq_c, val, atom.getKind() );
    if( ires!=0 ){
      Node realPart;
      if( Trace.isOn("cbqi-inst-debug") ){
        Trace("cbqi-inst-debug") << "Isolate : ";
        if( !veq_c.isNull() ){
          Trace("cbqi-inst-debug") << veq_c << " * ";
        }
        Trace("cbqi-inst-debug") << pv << " " << atom.getKind() << " " << val << std::endl;
      }
      if( options::cbqiAll() ){
        // when not pure LIA/LRA, we must check whether the lhs contains pv
        if( TermDb::containsTerm( val, pv ) ){
          Trace("cbqi-inst-debug") << "fail : contains bad term" << std::endl;
          return 0;
        }
      }
      if( pvtn.isInteger() && ( ( !veq_c.isNull() && !veq_c.getType().isInteger() ) || !val.getType().isInteger() ) ){
        //redo, split integer/non-integer parts
        bool useCoeff = false;
        Integer coeff = d_one.getConst<Rational>().getNumerator();
        for( std::map< Node, Node >::iterator it = msum.begin(); it != msum.end(); ++it ){
          if( it->first.isNull() || it->first.getType().isInteger() ){
            if( !it->second.isNull() ){
              coeff = coeff.lcm( it->second.getConst<Rational>().getDenominator() );
              useCoeff = true;
            }
          }
        }
        //multiply everything by this coefficient
        Node rcoeff = NodeManager::currentNM()->mkConst( Rational( coeff ) );
        std::vector< Node > real_part;
        for( std::map< Node, Node >::iterator it = msum.begin(); it != msum.end(); ++it ){
          if( useCoeff ){
            if( it->second.isNull() ){
              msum[it->first] = rcoeff;
            }else{
              msum[it->first] = Rewriter::rewrite( NodeManager::currentNM()->mkNode( MULT, it->second, rcoeff ) );
            }
          }
          if( !it->first.isNull() && !it->first.getType().isInteger() ){
            real_part.push_back( msum[it->first].isNull() ? it->first : NodeManager::currentNM()->mkNode( MULT, msum[it->first], it->first ) );
          }
        }
        //remove delta  TODO: check this
        vts_coeff[1] = Node::null();
        //multiply inf
        if( !vts_coeff[0].isNull() ){
          vts_coeff[0] = Rewriter::rewrite( NodeManager::currentNM()->mkNode( MULT, rcoeff, vts_coeff[0] ) );
        }
        realPart = real_part.empty() ? d_zero : ( real_part.size()==1 ? real_part[0] : NodeManager::currentNM()->mkNode( PLUS, real_part ) );
        Assert( d_out->isEligibleForInstantiation( realPart ) );
        //re-isolate
        Trace("cbqi-inst-debug") << "Re-isolate..." << std::endl;
        ires = QuantArith::isolate( pv, msum, veq_c, val, atom.getKind() );
        Trace("cbqi-inst-debug") << "Isolate for mixed Int/Real : " << veq_c << " * " << pv << " " << atom.getKind() << " " << val << std::endl;
        Trace("cbqi-inst-debug") << "                 real part : " << realPart << std::endl;
        if( ires!=0 ){
          int ires_use = ( msum[pv].isNull() || msum[pv].getConst<Rational>().sgn()==1 ) ? 1 : -1;
          val = Rewriter::rewrite( NodeManager::currentNM()->mkNode( ires_use==-1 ? PLUS : MINUS,
                                    NodeManager::currentNM()->mkNode( ires_use==-1 ? MINUS : PLUS, val, realPart ),
                                    NodeManager::currentNM()->mkNode( TO_INTEGER, realPart ) ) );  //TODO: round up for upper bounds?
          Trace("cbqi-inst-debug") << "result : " << val << std::endl;
          Assert( val.getType().isInteger() );
        }
      }
    }
    vts_coeff_inf = vts_coeff[0];
    vts_coeff_delta = vts_coeff[1];
    Trace("cbqi-inst-debug") << "Return " << veq_c << " * " << pv << " " << atom.getKind() << " " << val << ", vts = (" << vts_coeff_inf << ", " << vts_coeff_delta << ")" << std::endl;
  }else{
    Trace("cbqi-inst-debug") << "fail : could not get monomial sum" << std::endl;
  }
  return ires;
}

Node CegInstantiator::solve_dt( Node v, Node a, Node b, Node sa, Node sb ) {
  Trace("cbqi-inst-debug2") << "Solve dt : " << v << " " << a << " " << b << " " << sa << " " << sb << std::endl;
  Node ret;
  if( !a.isNull() && a==v ){
    ret = sb;
  }else if( !b.isNull() && b==v ){
    ret = sa;
  }else if( !a.isNull() && a.getKind()==APPLY_CONSTRUCTOR ){
    if( !b.isNull() && b.getKind()==APPLY_CONSTRUCTOR ){
      if( a.getOperator()==b.getOperator() ){
        for( unsigned i=0; i<a.getNumChildren(); i++ ){
          Node s = solve_dt( v, a[i], b[i], sa[i], sb[i] );
          if( !s.isNull() ){
            return s;
          }
        }
      }
    }else{
      unsigned cindex = Datatype::indexOf( a.getOperator().toExpr() );
      TypeNode tn = a.getType();
      const Datatype& dt = ((DatatypeType)(tn).toType()).getDatatype();
      for( unsigned i=0; i<a.getNumChildren(); i++ ){
        Node nn = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[cindex][i].getSelector() ), sb );
        Node s = solve_dt( v, a[i], Node::null(), sa[i], nn );
        if( !s.isNull() ){
          return s;
        }
      }
    }
  }else if( !b.isNull() && b.getKind()==APPLY_CONSTRUCTOR ){
    return solve_dt( v, b, a, sb, sa );
  }
  if( !ret.isNull() ){
    //ensure does not contain
    if( TermDb::containsTerm( ret, v ) ){
      ret = Node::null();
    }
  }
  return ret;
}




Instantiator::Instantiator( QuantifiersEngine * qe, TypeNode tn ) : d_type( tn ){
  d_closed_enum_type = qe->getTermDatabase()->isClosedEnumerableType( tn );
}


bool Instantiator::processEqualTerm( CegInstantiator * ci, SolvedForm& sf, Node pv, Node pv_coeff, Node n, unsigned effort ) {
  return ci->doAddInstantiationInc( pv, n, pv_coeff, 0, sf, effort );
}


void ArithInstantiator::reset( Node pv, unsigned effort ) {

}

bool ArithInstantiator::processEquality( CegInstantiator * ci, SolvedForm& sf, Node pv, std::vector< Node >& term_coeffs, std::vector< Node >& terms, unsigned effort ) {
  return false;
}

bool ArithInstantiator::processAssertion( CegInstantiator * ci, SolvedForm& sf, Node pv, Node lit, unsigned effort ) {
  return false;
}

bool ArithInstantiator::processAssertions( CegInstantiator * ci, SolvedForm& sf, Node pv, std::vector< Node >& lits, unsigned effort ) {
  return false;
}

bool ArithInstantiator::needsPostProcessInstantiation( CegInstantiator * ci, SolvedForm& sf, unsigned effort ) {
  return !sf.d_has_coeff.empty(); 
}

bool ArithInstantiator::postProcessInstantiation( CegInstantiator * ci, SolvedForm& sf, unsigned effort ) {
  return true;
}

void DtInstantiator::reset( Node pv, unsigned effort ) {

}

bool DtInstantiator::processEqualTerms( CegInstantiator * ci, SolvedForm& sf, Node pv, std::vector< Node >& eqc, unsigned effort ) {
  Trace("cbqi-inst-debug") << "[2] try based on constructors in equivalence class." << std::endl;
  //[2] look in equivalence class for a constructor
  for( unsigned k=0; k<eqc.size(); k++ ){
    Node n = eqc[k];
    if( n.getKind()==APPLY_CONSTRUCTOR ){
      Trace("cbqi-inst-debug") << "...try based on constructor term " << n << std::endl;
      std::vector< Node > children;
      children.push_back( n.getOperator() );
      const Datatype& dt = ((DatatypeType)(d_type).toType()).getDatatype();
      unsigned cindex = Datatype::indexOf( n.getOperator().toExpr() );
      //now must solve for selectors applied to pv
      for( unsigned j=0; j<dt[cindex].getNumArgs(); j++ ){
        Node c = NodeManager::currentNM()->mkNode( APPLY_SELECTOR_TOTAL, Node::fromExpr( dt[cindex][j].getSelector() ), pv );
        ci->pushStackVariable( c );
        children.push_back( c );
      }
      Node val = NodeManager::currentNM()->mkNode( kind::APPLY_CONSTRUCTOR, children );
      if( ci->doAddInstantiationInc( pv, val, Node::null(), 0, sf, effort ) ){
        return true;
      }else{
        //cleanup
        for( unsigned j=0; j<dt[cindex].getNumArgs(); j++ ){
          ci->popStackVariable();
        }
        break;
      }
    }
  }
  return false;
}

bool DtInstantiator::processEquality( CegInstantiator * ci, SolvedForm& sf, Node pv, std::vector< Node >& term_coeffs, std::vector< Node >& terms, unsigned effort ) {
  return false;
}

void EprInstantiator::reset( Node pv, unsigned effort ) {
  d_equal_terms.clear();
}

bool EprInstantiator::processEqualTerm( CegInstantiator * ci, SolvedForm& sf, Node pv, Node pv_coeff, Node n, unsigned effort ) {
  if( options::quantEprMatching() ){
    Assert( pv_coeff.isNull() );
    d_equal_terms.push_back( n ); 
    return false;  
  }else{
    return ci->doAddInstantiationInc( pv, n, pv_coeff, 0, sf, effort );
  }
}

void EprInstantiator::computeMatchScore( CegInstantiator * ci, Node pv, Node catom, std::vector< Node >& arg_reps, TermArgTrie * tat, unsigned index, std::map< Node, int >& match_score ) {
  if( index==catom.getNumChildren() ){
    Assert( tat->hasNodeData() );
    Node gcatom = tat->getNodeData();
    Trace("epr-inst") << "Matched : " << catom << " and " << gcatom << std::endl;
    for( unsigned i=0; i<catom.getNumChildren(); i++ ){
      if( catom[i]==pv ){
        Trace("epr-inst") << "...increment " << gcatom[i] << std::endl;
        match_score[gcatom[i]]++;
      }else{
        //recursive matching
        computeMatchScore( ci, pv, catom[i], gcatom[i], match_score );
      }
    }
  }else{
    std::map< TNode, TermArgTrie >::iterator it = tat->d_data.find( arg_reps[index] );
    if( it!=tat->d_data.end() ){
      computeMatchScore( ci, pv, catom, arg_reps, &it->second, index+1, match_score );
    }
  }
}

void EprInstantiator::computeMatchScore( CegInstantiator * ci, Node pv, Node catom, Node eqc, std::map< Node, int >& match_score ) {
  if( inst::Trigger::isAtomicTrigger( catom ) && TermDb::containsTerm( catom, pv ) ){
    Trace("epr-inst") << "Find matches for " << catom << "..." << std::endl;
    std::vector< Node > arg_reps;
    for( unsigned j=0; j<catom.getNumChildren(); j++ ){
      arg_reps.push_back( ci->getQuantifiersEngine()->getMasterEqualityEngine()->getRepresentative( catom[j] ) );
    }
    if( ci->getQuantifiersEngine()->getMasterEqualityEngine()->hasTerm( eqc ) ){
      Node rep = ci->getQuantifiersEngine()->getMasterEqualityEngine()->getRepresentative( eqc );
      Node op = ci->getQuantifiersEngine()->getTermDatabase()->getMatchOperator( catom );
      TermArgTrie * tat = ci->getQuantifiersEngine()->getTermDatabase()->getTermArgTrie( rep, op );
      Trace("epr-inst") << "EPR instantiation match term : " << catom << ", check ground terms=" << (tat!=NULL) << std::endl;
      if( tat ){
        computeMatchScore( ci, pv, catom, arg_reps, tat, 0, match_score );
      }
    }
  }
}

struct sortEqTermsMatch {
  std::map< Node, int > d_match_score;
  bool operator() (Node i, Node j) {
    int match_score_i = d_match_score[i];
    int match_score_j = d_match_score[j];
    return match_score_i>match_score_j || ( match_score_i==match_score_j && i<j );
  }
};

    
bool EprInstantiator::processEqualTerms( CegInstantiator * ci, SolvedForm& sf, Node pv, std::vector< Node >& eqc, unsigned effort ) {
  if( options::quantEprMatching() ){
    //heuristic for best matching constant
    sortEqTermsMatch setm;
    for( unsigned i=0; i<ci->getNumCEAtoms(); i++ ){
      Node catom = ci->getCEAtom( i );
      computeMatchScore( ci, pv, catom, catom, setm.d_match_score );
    }
    //sort by match score
    std::sort( d_equal_terms.begin(), d_equal_terms.end(), setm );
    Node pv_coeff;
    for( unsigned i=0; i<d_equal_terms.size(); i++ ){
      if( ci->doAddInstantiationInc( pv, d_equal_terms[i], pv_coeff, 0, sf, effort ) ){
        return true;
      }
    }
  }
  return false;
}

bool BvInstantiator::processAssertion( CegInstantiator * ci, SolvedForm& sf, Node pv, Node lit, unsigned effort ) {
  /*   TODO: algebraic reasoning for bitvector instantiation
  if( atom.getKind()==BITVECTOR_ULT || atom.getKind()==BITVECTOR_ULE ){
    for( unsigned t=0; t<2; t++ ){
      if( atom[t]==pv ){
        computeProgVars( atom[1-t] );
        if( d_inelig.find( atom[1-t] )==d_inelig.end() ){
          //only ground terms  TODO: more
          if( d_prog_var[atom[1-t]].empty() ){
            Node veq_c;
            Node uval;
            if( ( !pol && atom.getKind()==BITVECTOR_ULT ) || ( pol && atom.getKind()==BITVECTOR_ULE ) ){
              uval = atom[1-t];
            }else{
              uval = NodeManager::currentNM()->mkNode( (atom.getKind()==BITVECTOR_ULT)==(t==1) ? BITVECTOR_PLUS : BITVECTOR_SUB, atom[1-t], 
                                                       bv::utils::mkConst(pvtn.getConst<BitVectorSize>(), 1) );
            }
            if( doAddInstantiationInc( pv, uval, veq_c, 0, sf, effort ) ){
              return true;
            }
          }
        }
      }
    }
  }
  */

  return false;
}

