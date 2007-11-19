// Statements: D -> LLVM glue

#include <stdio.h>
#include <math.h>
#include <sstream>
#include <fstream>
#include <iostream>

#include "gen/llvm.h"

#include "total.h"
#include "init.h"
#include "mtype.h"
#include "hdrgen.h"
#include "port.h"

#include "gen/irstate.h"
#include "gen/logger.h"
#include "gen/tollvm.h"
#include "gen/runtime.h"
#include "gen/arrays.h"
#include "gen/todebug.h"
#include "gen/dvalue.h"

//////////////////////////////////////////////////////////////////////////////

void CompoundStatement::toIR(IRState* p)
{
    Logger::println("CompoundStatement::toIR()");
    LOG_SCOPE;

    for (int i=0; i<statements->dim; i++)
    {
        Statement* s = (Statement*)statements->data[i];
        if (s)
            s->toIR(p);
        else {
            Logger::println("??? null statement found in CompoundStatement");
        }
    }
}

//////////////////////////////////////////////////////////////////////////////

void ReturnStatement::toIR(IRState* p)
{
    static int rsi = 0;
    Logger::println("ReturnStatement::toIR(%d): %s", rsi++, toChars());
    LOG_SCOPE;

    if (exp)
    {
        Logger::println("return type is: %s", exp->type->toChars());

        Type* exptype = DtoDType(exp->type);
        TY expty = exptype->ty;
        if (p->topfunc()->getReturnType() == llvm::Type::VoidTy) {
            assert(DtoIsPassedByRef(exptype));

            TypeFunction* f = p->topfunctype();
            assert(f->llvmRetInPtr && f->llvmRetArg);

            if (global.params.symdebug) DtoDwarfStopPoint(loc.linnum);

            DValue* rvar = new DVarValue(f->next, f->llvmRetArg, true);

            p->exps.push_back(IRExp(NULL,exp,rvar));
            DValue* e = exp->toElem(p);
            p->exps.pop_back();

            if (!e->inPlace())
                DtoAssign(rvar, e);

            IRFunction::FinallyVec& fin = p->func()->finallys;
            if (fin.empty()) {
                if (global.params.symdebug) DtoDwarfFuncEnd(p->func()->decl);
                new llvm::ReturnInst(p->scopebb());
            }
            else {
                new llvm::BranchInst(fin.back().retbb, p->scopebb());
            }
        }
        else {
            if (global.params.symdebug) DtoDwarfStopPoint(loc.linnum);
            DValue* e = exp->toElem(p);
            llvm::Value* v = e->getRVal();
            delete e;
            Logger::cout() << "return value is '" <<*v << "'\n";

            IRFunction::FinallyVec& fin = p->func()->finallys;
            if (fin.empty()) {
                if (global.params.symdebug) DtoDwarfFuncEnd(p->func()->decl);
                new llvm::ReturnInst(v, p->scopebb());
            }
            else {
                if (!p->func()->finallyretval)
                    p->func()->finallyretval = new llvm::AllocaInst(v->getType(),"tmpreturn",p->topallocapoint());
                llvm::Value* rettmp = p->func()->finallyretval;
                new llvm::StoreInst(v,rettmp,p->scopebb());
                new llvm::BranchInst(fin.back().retbb, p->scopebb());
            }
        }
    }
    else
    {
        if (p->topfunc()->getReturnType() == llvm::Type::VoidTy) {
            IRFunction::FinallyVec& fin = p->func()->finallys;
            if (fin.empty()) {
                if (global.params.symdebug) DtoDwarfFuncEnd(p->func()->decl);
                new llvm::ReturnInst(p->scopebb());
            }
            else {
                new llvm::BranchInst(fin.back().retbb, p->scopebb());
            }
        }
        else {
            assert(0); // why should this ever happen?
            new llvm::UnreachableInst(p->scopebb());
        }
    }
}

//////////////////////////////////////////////////////////////////////////////

void ExpStatement::toIR(IRState* p)
{
    static int esi = 0;
    Logger::println("ExpStatement::toIR(%d): %s", esi++, toChars());
    LOG_SCOPE;

//     if (global.params.symdebug)
//         DtoDwarfStopPoint(loc.linnum);

    if (exp != 0) {
        elem* e = exp->toElem(p);
        delete e;
    }
    /*elem* e = exp->toElem(p);
    p->buf.printf("%s", e->toChars());
    delete e;
    p->buf.writenl();*/
}

//////////////////////////////////////////////////////////////////////////////

void IfStatement::toIR(IRState* p)
{
    static int wsi = 0;
    Logger::println("IfStatement::toIR(%d): %s", wsi++, toChars());
    LOG_SCOPE;

    DValue* cond_e = condition->toElem(p);
    llvm::Value* cond_val = cond_e->getRVal();
    delete cond_e;

    llvm::BasicBlock* oldend = gIR->scopeend();

    llvm::BasicBlock* ifbb = new llvm::BasicBlock("if", gIR->topfunc(), oldend);
    llvm::BasicBlock* endbb = new llvm::BasicBlock("endif", gIR->topfunc(), oldend);
    llvm::BasicBlock* elsebb = elsebody ? new llvm::BasicBlock("else", gIR->topfunc(), endbb) : endbb;

    if (cond_val->getType() != llvm::Type::Int1Ty) {
        Logger::cout() << "if conditional: " << *cond_val << '\n';
        cond_val = DtoBoolean(cond_val);
    }
    llvm::Value* ifgoback = new llvm::BranchInst(ifbb, elsebb, cond_val, gIR->scopebegin());

    // replace current scope
    gIR->scope() = IRScope(ifbb,elsebb);

    // do scoped statements
    ifbody->toIR(p);
    if (!gIR->scopereturned()) {
        new llvm::BranchInst(endbb,gIR->scopebegin());
    }

    if (elsebody) {
        //assert(0);
        gIR->scope() = IRScope(elsebb,endbb);
        elsebody->toIR(p);
        if (!gIR->scopereturned()) {
            new llvm::BranchInst(endbb,gIR->scopebegin());
        }
    }

    // rewrite the scope
    gIR->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void ScopeStatement::toIR(IRState* p)
{
    Logger::println("ScopeStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    llvm::BasicBlock* oldend = p->scopeend();

    llvm::BasicBlock* beginbb = 0;

    // remove useless branches by clearing and reusing the current basicblock
    llvm::BasicBlock* bb = p->scopebegin();
    if (bb->empty()) {
        beginbb = bb;
    }
    else {
        assert(!p->scopereturned());
        beginbb = new llvm::BasicBlock("scope", p->topfunc(), oldend);
        new llvm::BranchInst(beginbb, p->scopebegin());
    }
    llvm::BasicBlock* endbb = new llvm::BasicBlock("endscope", p->topfunc(), oldend);

    gIR->scope() = IRScope(beginbb, endbb);

    statement->toIR(p);

    p->scope() = IRScope(p->scopebb(),oldend);
    endbb->eraseFromParent();
}

//////////////////////////////////////////////////////////////////////////////

void WhileStatement::toIR(IRState* p)
{
    static int wsi = 0;
    Logger::println("WhileStatement::toIR(%d): %s", wsi++, toChars());
    LOG_SCOPE;

    // create while blocks
    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* whilebb = new llvm::BasicBlock("whilecond", gIR->topfunc(), oldend);
    llvm::BasicBlock* whilebodybb = new llvm::BasicBlock("whilebody", gIR->topfunc(), oldend);
    llvm::BasicBlock* endbb = new llvm::BasicBlock("endwhile", gIR->topfunc(), oldend);

    // move into the while block
    p->ir->CreateBr(whilebb);
    //new llvm::BranchInst(whilebb, gIR->scopebegin());

    // replace current scope
    gIR->scope() = IRScope(whilebb,endbb);

    // create the condition
    DValue* cond_e = condition->toElem(p);
    llvm::Value* cond_val = DtoBoolean(cond_e->getRVal());
    delete cond_e;

    // conditional branch
    llvm::Value* ifbreak = new llvm::BranchInst(whilebodybb, endbb, cond_val, p->scopebb());

    // rewrite scope
    gIR->scope() = IRScope(whilebodybb,endbb);

    // while body code
    p->loopbbs.push_back(IRScope(whilebb,endbb));
    body->toIR(p);
    p->loopbbs.pop_back();

    // loop
    new llvm::BranchInst(whilebb, gIR->scopebegin());

    // rewrite the scope
    gIR->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void DoStatement::toIR(IRState* p)
{
    static int wsi = 0;
    Logger::println("DoStatement::toIR(%d): %s", wsi++, toChars());
    LOG_SCOPE;

    // create while blocks
    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* dowhilebb = new llvm::BasicBlock("dowhile", gIR->topfunc(), oldend);
    llvm::BasicBlock* endbb = new llvm::BasicBlock("enddowhile", gIR->topfunc(), oldend);

    // move into the while block
    new llvm::BranchInst(dowhilebb, gIR->scopebegin());

    // replace current scope
    gIR->scope() = IRScope(dowhilebb,endbb);

    // do-while body code
    body->toIR(p);

    // create the condition
    DValue* cond_e = condition->toElem(p);
    llvm::Value* cond_val = DtoBoolean(cond_e->getRVal());
    delete cond_e;

    // conditional branch
    llvm::Value* ifbreak = new llvm::BranchInst(dowhilebb, endbb, cond_val, gIR->scopebegin());

    // rewrite the scope
    gIR->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void ForStatement::toIR(IRState* p)
{
    static int wsi = 0;
    Logger::println("ForStatement::toIR(%d): %s", wsi++, toChars());
    LOG_SCOPE;

    // create for blocks
    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* forbb = new llvm::BasicBlock("forcond", gIR->topfunc(), oldend);
    llvm::BasicBlock* forbodybb = new llvm::BasicBlock("forbody", gIR->topfunc(), oldend);
    llvm::BasicBlock* forincbb = new llvm::BasicBlock("forinc", gIR->topfunc(), oldend);
    llvm::BasicBlock* endbb = new llvm::BasicBlock("endfor", gIR->topfunc(), oldend);

    // init
    if (init != 0)
    init->toIR(p);

    // move into the for condition block, ie. start the loop
    new llvm::BranchInst(forbb, gIR->scopebegin());

    p->loopbbs.push_back(IRScope(forincbb,endbb));

    // replace current scope
    gIR->scope() = IRScope(forbb,forbodybb);

    // create the condition
    DValue* cond_e = condition->toElem(p);
    llvm::Value* cond_val = DtoBoolean(cond_e->getRVal());
    delete cond_e;

    // conditional branch
    llvm::Value* ifbreak = new llvm::BranchInst(forbodybb, endbb, cond_val, forbb);

    // rewrite scope
    gIR->scope() = IRScope(forbodybb,forincbb);

    // do for body code
    body->toIR(p);

    // move into the for increment block
    new llvm::BranchInst(forincbb, gIR->scopebegin());
    gIR->scope() = IRScope(forincbb, endbb);

    // increment
    if (increment) {
        DValue* inc = increment->toElem(p);
        delete inc;
    }

    // loop
    new llvm::BranchInst(forbb, gIR->scopebegin());

    p->loopbbs.pop_back();

    // rewrite the scope
    gIR->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void BreakStatement::toIR(IRState* p)
{
    Logger::println("BreakStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    if (ident != 0) {
        Logger::println("ident = %s", ident->toChars());
        assert(0);
    }
    else {
        new llvm::BranchInst(gIR->loopbbs.back().end, gIR->scopebegin());
    }
}

//////////////////////////////////////////////////////////////////////////////

void ContinueStatement::toIR(IRState* p)
{
    Logger::println("ContinueStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    if (ident != 0) {
        Logger::println("ident = %s", ident->toChars());
        assert(0);
    }
    else {
        new llvm::BranchInst(gIR->loopbbs.back().begin, gIR->scopebegin());
    }
}

//////////////////////////////////////////////////////////////////////////////

void OnScopeStatement::toIR(IRState* p)
{
    Logger::println("OnScopeStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    assert(statement);
    //statement->toIR(p); // this seems to be redundant
}

//////////////////////////////////////////////////////////////////////////////

void TryFinallyStatement::toIR(IRState* p)
{
    Logger::println("TryFinallyStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    // create basic blocks
    llvm::BasicBlock* oldend = p->scopeend();

    llvm::BasicBlock* trybb = new llvm::BasicBlock("try", p->topfunc(), oldend);
    llvm::BasicBlock* finallybb = new llvm::BasicBlock("finally", p->topfunc(), oldend);
    llvm::BasicBlock* finallyretbb = new llvm::BasicBlock("finallyreturn", p->topfunc(), oldend);
    llvm::BasicBlock* endbb = new llvm::BasicBlock("endtryfinally", p->topfunc(), oldend);

    // pass the previous BB into this
    assert(!gIR->scopereturned());
    new llvm::BranchInst(trybb, p->scopebb());

    // do the try block
    p->scope() = IRScope(trybb,finallybb);
    gIR->func()->finallys.push_back(IRFinally(finallybb,finallyretbb));
    IRFinally& fin = p->func()->finallys.back();

    assert(body);
    body->toIR(p);

    // terminate try BB
    if (!p->scopereturned())
        new llvm::BranchInst(finallybb, p->scopebb());

    // do finally block
    p->scope() = IRScope(finallybb,finallyretbb);
    assert(finalbody);
    finalbody->toIR(p);

    // terminate finally
    if (!gIR->scopereturned()) {
        new llvm::BranchInst(endbb, p->scopebb());
    }

    // do finally block (return path)
    p->scope() = IRScope(finallyretbb,endbb);
    assert(finalbody);
    finalbody->toIR(p); // hope this will work, otherwise it's time it gets fixed

    // terminate finally (return path)
    size_t nfin = p->func()->finallys.size();
    if (nfin > 1) {
        IRFinally& ofin = p->func()->finallys[nfin-2];
        p->ir->CreateBr(ofin.retbb);
    }
    // no outer
    else
    {
        if (global.params.symdebug) DtoDwarfFuncEnd(p->func()->decl);
        llvm::Value* retval = p->func()->finallyretval;
        if (retval) {
            retval = p->ir->CreateLoad(retval,"tmp");
            p->ir->CreateRet(retval);
        }
        else {
            FuncDeclaration* fd = p->func()->decl;
            if (fd->isMain()) {
                assert(fd->type->next->ty == Tvoid);
                p->ir->CreateRet(DtoConstInt(0));
            }
            else {
                p->ir->CreateRetVoid();
            }
        }
    }

    // rewrite the scope
    p->func()->finallys.pop_back();
    p->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void TryCatchStatement::toIR(IRState* p)
{
    static int wsi = 0;
    Logger::println("TryCatchStatement::toIR(%d): %s", wsi++, toChars());
    LOG_SCOPE;

    Logger::attention("try-catch is not yet fully implemented, only the try block will be emitted.");

    assert(body);
    body->toIR(p);

    /*assert(catches);
    for(size_t i=0; i<catches->dim; ++i)
    {
        Catch* c = (Catch*)catches->data[i];
        c->handler->toIR(p);
    }*/
}

//////////////////////////////////////////////////////////////////////////////

void ThrowStatement::toIR(IRState* p)
{
    static int wsi = 0;
    Logger::println("ThrowStatement::toIR(%d): %s", wsi++, toChars());
    LOG_SCOPE;

    Logger::attention("throw is not yet implemented, replacing expression with assert(0);");

    DtoAssert(NULL, &loc, NULL);

    /*
    assert(exp);
    DValue* e = exp->toElem(p);
    delete e;
    */
}

//////////////////////////////////////////////////////////////////////////////

void SwitchStatement::toIR(IRState* p)
{
    Logger::println("SwitchStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    llvm::BasicBlock* oldend = gIR->scopeend();

    // collect the needed cases
    typedef std::pair<llvm::BasicBlock*, llvm::ConstantInt*> CasePair;
    std::vector<CasePair> vcases;
    for (int i=0; i<cases->dim; ++i)
    {
        CaseStatement* cs = (CaseStatement*)cases->data[i];

        // get the case value
        DValue* e = cs->exp->toElem(p);
        DConstValue* ce = e->isConst();
        assert(ce);
        llvm::ConstantInt* ec = isaConstantInt(ce->c);
        assert(ec);
        delete e;

        // create the case bb with a nice label
        std::string lblname("case"+std::string(cs->exp->toChars()));
        llvm::BasicBlock* bb = new llvm::BasicBlock(lblname, p->topfunc(), oldend);

        vcases.push_back(CasePair(bb,ec));
    }

    // default
    llvm::BasicBlock* defbb = 0;
    if (!hasNoDefault) {
        defbb = new llvm::BasicBlock("default", p->topfunc(), oldend);
    }

    // end (break point)
    llvm::BasicBlock* endbb = new llvm::BasicBlock("switchend", p->topfunc(), oldend);

    // condition var
    DValue* cond = condition->toElem(p);
    llvm::SwitchInst* si = new llvm::SwitchInst(cond->getRVal(), defbb ? defbb : endbb, cases->dim, p->scopebb());
    delete cond;

    // add the cases
    size_t n = vcases.size();
    for (size_t i=0; i<n; ++i)
    {
        si->addCase(vcases[i].second, vcases[i].first);
    }

    // insert case statements
    for (size_t i=0; i<n; ++i)
    {
        llvm::BasicBlock* nextbb = (i == n-1) ? (defbb ? defbb : endbb) : vcases[i+1].first;
        p->scope() = IRScope(vcases[i].first,nextbb);

        p->loopbbs.push_back(IRScope(p->scopebb(),endbb));
        static_cast<CaseStatement*>(cases->data[i])->statement->toIR(p);
        p->loopbbs.pop_back();

        llvm::BasicBlock* curbb = p->scopebb();
        if (curbb->empty() || !curbb->back().isTerminator())
        {
            new llvm::BranchInst(nextbb, curbb);
        }
    }

    // default statement
    if (defbb)
    {
        p->scope() = IRScope(defbb,endbb);
        p->loopbbs.push_back(IRScope(defbb,endbb));
        Logger::println("doing default statement");
        sdefault->statement->toIR(p);
        p->loopbbs.pop_back();

        llvm::BasicBlock* curbb = p->scopebb();
        if (curbb->empty() || !curbb->back().isTerminator())
        {
            new llvm::BranchInst(endbb, curbb);
        }
    }

    gIR->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void UnrolledLoopStatement::toIR(IRState* p)
{
    Logger::println("UnrolledLoopStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* endbb = new llvm::BasicBlock("unrolledend", p->topfunc(), oldend);

    p->scope() = IRScope(p->scopebb(),endbb);
    p->loopbbs.push_back(IRScope(p->scopebb(),endbb));

    for (int i=0; i<statements->dim; ++i)
    {
        Statement* s = (Statement*)statements->data[i];
        s->toIR(p);
    }

    p->loopbbs.pop_back();

    new llvm::BranchInst(endbb, p->scopebb());
    p->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void ForeachStatement::toIR(IRState* p)
{
    Logger::println("ForeachStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    //assert(arguments->dim == 1);
    assert(value != 0);
    assert(body != 0);
    assert(aggr != 0);
    assert(func != 0);

    //Argument* arg = (Argument*)arguments->data[0];
    //Logger::println("Argument is %s", arg->toChars());

    Logger::println("aggr = %s", aggr->toChars());

    // key
    const llvm::Type* keytype = key ? DtoType(key->type) : DtoSize_t();
    llvm::Value* keyvar = new llvm::AllocaInst(keytype, "foreachkey", p->topallocapoint());
    if (key) key->llvmValue = keyvar;
    llvm::Value* zerokey = llvm::ConstantInt::get(keytype,0,false);

    // value
    const llvm::Type* valtype = DtoType(value->type);
    llvm::Value* valvar = NULL;
    if (!value->isRef() && !value->isOut())
        valvar = new llvm::AllocaInst(valtype, "foreachval", p->topallocapoint());

    // what to iterate
    DValue* aggrval = aggr->toElem(p);
    Type* aggrtype = DtoDType(aggr->type);

    // get length and pointer
    llvm::Value* val = 0;
    llvm::Value* niters = 0;

    // static array
    if (aggrtype->ty == Tsarray)
    {
        Logger::println("foreach over static array");
        val = aggrval->getRVal();
        assert(isaPointer(val->getType()));
        const llvm::ArrayType* arrty = isaArray(val->getType()->getContainedType(0));
        assert(arrty);
        size_t nelems = arrty->getNumElements();
        assert(nelems > 0);
        niters = llvm::ConstantInt::get(keytype,nelems,false);
    }
    // dynamic array
    else if (aggrtype->ty == Tarray)
    {
        if (DSliceValue* slice = aggrval->isSlice()) {
            Logger::println("foreach over slice");
            niters = slice->len;
            assert(niters);
            val = slice->ptr;
            assert(val);
        }
        else {
            Logger::println("foreach over dynamic array");
            val = aggrval->getRVal();
            niters = DtoGEPi(val,0,0,"tmp",p->scopebb());
            niters = p->ir->CreateLoad(niters, "numiterations");
            val = DtoGEPi(val,0,1,"tmp",p->scopebb());
            val = p->ir->CreateLoad(val, "collection");
        }
    }
    else
    {
        assert(0 && "aggregate type is not Tarray or Tsarray");
    }

    llvm::Constant* delta = 0;
    if (op == TOKforeach) {
        new llvm::StoreInst(zerokey, keyvar, p->scopebb());
    }
    else {
        new llvm::StoreInst(niters, keyvar, p->scopebb());
    }

    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* condbb = new llvm::BasicBlock("foreachcond", p->topfunc(), oldend);
    llvm::BasicBlock* bodybb = new llvm::BasicBlock("foreachbody", p->topfunc(), oldend);
    llvm::BasicBlock* nextbb = new llvm::BasicBlock("foreachnext", p->topfunc(), oldend);
    llvm::BasicBlock* endbb = new llvm::BasicBlock("foreachend", p->topfunc(), oldend);

    new llvm::BranchInst(condbb, p->scopebb());

    // condition
    p->scope() = IRScope(condbb,bodybb);

    llvm::Value* done = 0;
    llvm::Value* load = new llvm::LoadInst(keyvar, "tmp", p->scopebb());
    if (op == TOKforeach) {
        done = new llvm::ICmpInst(llvm::ICmpInst::ICMP_ULT, load, niters, "tmp", p->scopebb());
    }
    else if (op == TOKforeach_reverse) {
        done = new llvm::ICmpInst(llvm::ICmpInst::ICMP_UGT, load, zerokey, "tmp", p->scopebb());
        load = llvm::BinaryOperator::createSub(load,llvm::ConstantInt::get(keytype, 1, false),"tmp",p->scopebb());
        new llvm::StoreInst(load, keyvar, p->scopebb());
    }
    new llvm::BranchInst(bodybb, endbb, done, p->scopebb());

    // body
    p->scope() = IRScope(bodybb,nextbb);

    // get value for this iteration
    llvm::Constant* zero = llvm::ConstantInt::get(keytype,0,false);
    llvm::Value* loadedKey = p->ir->CreateLoad(keyvar,"tmp");
    if (aggrtype->ty == Tsarray)
        value->llvmValue = DtoGEP(val,zero,loadedKey,"tmp");
    else if (aggrtype->ty == Tarray)
        value->llvmValue = new llvm::GetElementPtrInst(val,loadedKey,"tmp",p->scopebb());

    if (!value->isRef() && !value->isOut()) {
        DValue* dst = new DVarValue(value->type, valvar, true);
        DValue* src = new DVarValue(value->type, value->llvmValue, true);
        DtoAssign(dst, src);
        delete dst;
        delete src;
        value->llvmValue = valvar;
    }

    // body
    p->loopbbs.push_back(IRScope(nextbb,endbb));
    body->toIR(p);
    p->loopbbs.pop_back();

    if (!p->scopereturned())
        new llvm::BranchInst(nextbb, p->scopebb());

    // next
    p->scope() = IRScope(nextbb,endbb);
    if (op == TOKforeach) {
        llvm::Value* load = DtoLoad(keyvar);
        load = p->ir->CreateAdd(load, llvm::ConstantInt::get(keytype, 1, false), "tmp");
        DtoStore(load, keyvar);
    }
    new llvm::BranchInst(condbb, p->scopebb());

    // end
    p->scope() = IRScope(endbb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void LabelStatement::toIR(IRState* p)
{
    Logger::println("LabelStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    assert(tf == NULL);
    assert(!isReturnLabel);

    llvm::BasicBlock* oldend = gIR->scopeend();
    if (llvmBB)
        llvmBB->moveBefore(oldend);
    else
        llvmBB = new llvm::BasicBlock("label", p->topfunc(), oldend);

    if (!p->scopereturned())
        new llvm::BranchInst(llvmBB, p->scopebb());

    p->scope() = IRScope(llvmBB,oldend);
    if (statement)
        statement->toIR(p);
}

//////////////////////////////////////////////////////////////////////////////

void GotoStatement::toIR(IRState* p)
{
    Logger::println("GotoStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    assert(tf == NULL);

    llvm::BasicBlock* oldend = gIR->scopeend();
    llvm::BasicBlock* bb = new llvm::BasicBlock("aftergoto", p->topfunc(), oldend);

    if (label->statement->llvmBB == NULL)
        label->statement->llvmBB = new llvm::BasicBlock("label", p->topfunc());
    assert(!p->scopereturned());
    new llvm::BranchInst(label->statement->llvmBB, p->scopebb());
    p->scope() = IRScope(bb,oldend);
}

//////////////////////////////////////////////////////////////////////////////

void WithStatement::toIR(IRState* p)
{
    Logger::println("WithStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    assert(exp);
    assert(body);

    DValue* e = exp->toElem(p);
    wthis->llvmValue = e->getRVal();
    delete e;

    body->toIR(p);
}

//////////////////////////////////////////////////////////////////////////////

void SynchronizedStatement::toIR(IRState* p)
{
    Logger::println("SynchronizedStatement::toIR(): %s", toChars());
    LOG_SCOPE;

    Logger::attention("synchronized is currently ignored. only the body will be emitted");

    body->toIR(p);
}

//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////

#define STUBST(x) void x::toIR(IRState * p) {error("Statement type "#x" not implemented: %s", toChars());fatal();}
//STUBST(BreakStatement);
//STUBST(ForStatement);
//STUBST(WithStatement);
//STUBST(SynchronizedStatement);
//STUBST(ReturnStatement);
//STUBST(ContinueStatement);
STUBST(DefaultStatement);
STUBST(CaseStatement);
//STUBST(SwitchStatement);
STUBST(SwitchErrorStatement);
STUBST(Statement);
//STUBST(IfStatement);
//STUBST(ForeachStatement);
//STUBST(DoStatement);
//STUBST(WhileStatement);
//STUBST(ExpStatement);
//STUBST(CompoundStatement);
//STUBST(ScopeStatement);
STUBST(AsmStatement);
//STUBST(TryCatchStatement);
//STUBST(TryFinallyStatement);
STUBST(VolatileStatement);
//STUBST(LabelStatement);
//STUBST(ThrowStatement);
STUBST(GotoCaseStatement);
STUBST(GotoDefaultStatement);
//STUBST(GotoStatement);
//STUBST(UnrolledLoopStatement);
//STUBST(OnScopeStatement);