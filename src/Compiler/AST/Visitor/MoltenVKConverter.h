/*
 * MoltenVKConverter.h
 *
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2017 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 * Author: Remo Eichenberger
 */

#ifndef XSC_MOLTENVK_CONVERTER_H
#define XSC_MOLTENVK_CONVERTER_H


#include "Visitor.h"
#include "Flags.h"
#include <Xsc/Xsc.h>
#include <functional>
#include <map>
#include <ast.h>


namespace Xsc
{

    // Convert buffers for MoltenVK.
    class MoltenVKConverter : public Visitor
    {

    public:

        void Convert(
            Program& program
        );

    private:

        /* ----- Conversion ----- */

        void ConvertExprType(Expr* expr);
        void ConvertExpr(const ExprPtr& expr);

        /* ----- Visitor implementation ----- */

        DECL_VISIT_PROC(ExprStmnt);
        DECL_VISIT_PROC(BufferDecl);
        DECL_VISIT_PROC(CallExpr);
        DECL_VISIT_PROC(ObjectExpr);
        DECL_VISIT_PROC(ArrayExpr);

        /* === Members === */
        
        bool onVisitBufferDecl(BufferDecl& bufDecl);

        bool            resetExprTypes_ = false;    // If true, all expression types must be reset.
        std::set<AST*>  convertedSymbols_;              // List of all symbols, whose type denoters have been reset.

    };


} // /namespace Xsc


#endif



// ================================================================================