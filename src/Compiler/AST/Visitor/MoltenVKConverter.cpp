/*
 * MoltenVKConverter.cpp
 *
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2017 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 * Author: Remo Eichenberger
  */

#include "MoltenVKConverter.h"
#include "AST.h"
#include <ASTFactory.h>

namespace Xsc
{
    void MoltenVKConverter::Convert(
        Program& program)
    {
        Visit(&program);
    }
    
    /*
     * ======= Private: =======
     */

    void MoltenVKConverter::ConvertExprType(Expr* expr)
    {
        if(expr && resetExprTypes_)
            expr->ResetTypeDenoter();
    }

    void MoltenVKConverter::ConvertExpr(const ExprPtr& expr)
    {
        if(expr)
        {
            /* Visit expression */
            Visit(expr);

            /* Check if type must be reset */
            if(resetExprTypes_)
            {
                expr->ResetTypeDenoter();
                resetExprTypes_ = false;
            }
        }
    }

    bool MoltenVKConverter::onVisitBufferDecl(BufferDecl& bufferDecl)
    {
        if(auto bufferDeclStmnt = bufferDecl.declStmntRef)
        {
            if(bufferDeclStmnt->typeDenoter->bufferType == BufferType::RWBuffer) {
                BufferTypeDenoterPtr newBufferTypeDen = std::make_shared<BufferTypeDenoter>(BufferType::RWStructuredBuffer);
                newBufferTypeDen->genericTypeDenoter = bufferDeclStmnt->typeDenoter->GetGenericTypeDenoter();
                newBufferTypeDen->genericSize = bufferDeclStmnt->typeDenoter->genericSize;
                bufferDeclStmnt->typeDenoter = newBufferTypeDen;
                bufferDecl.ResetTypeDenoter();
                return true;
            }
        }
        return false;
    }

    /* ------- Visit functions ------- */

#define IMPLEMENT_VISIT_PROC(AST_NAME) \
    void MoltenVKConverter::Visit##AST_NAME(AST_NAME* ast, void* args)

    IMPLEMENT_VISIT_PROC(ArrayExpr)
    {
        VISIT_DEFAULT(ArrayExpr);
        ConvertExprType(ast);
    }

    IMPLEMENT_VISIT_PROC(CallExpr)
    {
        VISIT_DEFAULT(CallExpr);
        ConvertExprType(ast);
    }

    IMPLEMENT_VISIT_PROC(ObjectExpr)
    {
        VISIT_DEFAULT(ObjectExpr);

        if(auto symbol = ast->symbolRef)
        {
            /* Check if referenced symbol has a converted type */
            if(convertedSymbols_.find(symbol) != convertedSymbols_.end())
                resetExprTypes_ = true;
        }
        ConvertExprType(ast);
    }

    IMPLEMENT_VISIT_PROC(BufferDecl)
    {
        VISIT_DEFAULT(BufferDecl);
        if(onVisitBufferDecl(*ast))
            convertedSymbols_.insert(ast);
    }

    IMPLEMENT_VISIT_PROC(ExprStmnt)
    {
        VISIT_DEFAULT(ExprStmnt);
        ConvertExpr(ast->expr);
    }

#undef IMPLEMENT_VISIT_PROC


} // /namespace Xsc



// ================================================================================