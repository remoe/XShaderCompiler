/*
 * GLSLConverter.cpp
 * 
 * This file is part of the XShaderCompiler project (Copyright (c) 2014-2017 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#include "GLSLConverter.h"
#include "GLSLKeywords.h"
#include "FuncNameConverter.h"
#include "AST.h"
#include "ASTFactory.h"
#include "Exception.h"
#include "Helper.h"
#include "ReportIdents.h"


namespace Xsc
{


/*
 * Internal structures
 */

struct CodeBlockStmntArgs
{
    bool disableNewScope;
};


/*
 * GLSLConverter class
 */

void GLSLConverter::Convert(
    Program& program, const ShaderTarget shaderTarget, const NameMangling& nameMangling, const Options& options, const OutputShaderVersion versionOut)
{
    /* Store settings */
    shaderTarget_       = shaderTarget;
    program_            = (&program);
    nameMangling_       = nameMangling;
    options_            = options;
    isVKSL_             = IsLanguageVKSL(versionOut);

    /* Convert expressions */
    Flags exprConverterFlags = ExprConverter::All;

    if ( isVKSL_ || ( versionOut >= OutputShaderVersion::GLSL420 && versionOut <= OutputShaderVersion::GLSL450 ))
    {
        /*
        Remove specific conversions when the GLSL output version is explicitly set to 4.20 or higher,
        i.e. "GL_ARB_shading_language_420pack" extension is available.
        */
        exprConverterFlags.Remove(ExprConverter::ConvertVectorSubscripts);
        exprConverterFlags.Remove(ExprConverter::ConvertInitializer);
    }

    exprConverter_.Convert(program, exprConverterFlags);

    /* Visit program AST */
    Visit(program_);

    /* Convert function names after main conversion, since functon owner structs may have been renamed as well */
    FuncNameConverter funcNameConverter;
    funcNameConverter.Convert(
        program,
        nameMangling_,
        GLSLConverter::CompareFuncSignatures,
        FuncNameConverter::All
    );
}


/*
 * ======= Private: =======
 */

/* ------- Visit functions ------- */

#define IMPLEMENT_VISIT_PROC(AST_NAME) \
    void GLSLConverter::Visit##AST_NAME(AST_NAME* ast, void* args)

IMPLEMENT_VISIT_PROC(Program)
{
    auto entryPoint = ast->entryPointRef;

    /* Register all input and output semantic variables as reserved identifiers */
    switch (shaderTarget_)
    {
        case ShaderTarget::VertexShader:
            if (nameMangling_.useAlwaysSemantics)
                RenameInOutVarIdents(entryPoint->inputSemantics.varDeclRefs, true, true);
            RenameInOutVarIdents(entryPoint->outputSemantics.varDeclRefs, false);
            break;

        case ShaderTarget::FragmentShader:
            RenameInOutVarIdents(entryPoint->inputSemantics.varDeclRefs, true);
            if (nameMangling_.useAlwaysSemantics)
                RenameInOutVarIdents(entryPoint->outputSemantics.varDeclRefs, false, true);
            break;

        default:
            RenameInOutVarIdents(entryPoint->inputSemantics.varDeclRefs, true);
            RenameInOutVarIdents(entryPoint->outputSemantics.varDeclRefs, false);
            break;
    }

    RegisterGlobalDeclIdents(entryPoint->inputSemantics.varDeclRefs);
    RegisterGlobalDeclIdents(entryPoint->outputSemantics.varDeclRefs);

    RegisterGlobalDeclIdents(entryPoint->inputSemantics.varDeclRefsSV);
    RegisterGlobalDeclIdents(entryPoint->outputSemantics.varDeclRefsSV);

    VISIT_DEFAULT(Program);

    //TODO: do not remove these statements, instead mark it as disabled code (otherwise symbol references to these statements are corrupted!)
    #if 1
    if (!isVKSL_)
    {
        /* Remove all variables which are sampler state objects, since GLSL does not support sampler states */
        MoveAllIf(
            ast->globalStmnts,
            program_->disabledAST,
            [&](const StmntPtr& stmnt)
            {
                if (stmnt->Type() == AST::Types::SamplerDeclStmnt)
                    return true;
                if (auto varDeclStmnt = stmnt->As<VarDeclStmnt>())
                    return IsSamplerStateTypeDenoter(varDeclStmnt->typeSpecifier->GetTypeDenoter());
                return false;
            }
        );
    }
    #endif
}

IMPLEMENT_VISIT_PROC(CodeBlock)
{
    RemoveDeadCode(ast->stmnts);

    UnrollStmnts(ast->stmnts);

    VISIT_DEFAULT(CodeBlock);
}

IMPLEMENT_VISIT_PROC(CallExpr)
{
    Visit(ast->prefixExpr);

    if (ast->intrinsic != Intrinsic::Undefined)
    {
        /* Insert prefix expression as first argument into function call, if this is a texture intrinsic call */
        if (IsTextureIntrinsic(ast->intrinsic) && ast->prefixExpr)
        {
            if (isVKSL_)
            {
                /* Replace sampler state argument by sampler/texture binding call */
                if (!ast->arguments.empty())
                {
                    auto arg0Expr = ast->arguments.front().get();
                    if (IsSamplerStateTypeDenoter(arg0Expr->GetTypeDenoter()))
                    {
                        ast->arguments[0] = ASTFactory::MakeTextureSamplerBindingCallExpr(
                            ast->prefixExpr, ast->arguments[0]
                        );
                    }
                }
            }
            else
            {
                /* Insert texture object as parameter into intrinsic arguments */
                ast->arguments.insert(ast->arguments.begin(), ast->prefixExpr);
            }
        }
    }

    if (!isVKSL_)
    {
        /* Remove arguments which contain a sampler state object, since GLSL does not support sampler states */
        MoveAllIf(
            ast->arguments,
            program_->disabledAST,
            [&](const ExprPtr& expr)
            {
                return IsSamplerStateTypeDenoter(expr->GetTypeDenoter());
            }
        );
    }

    if (ast->intrinsic != Intrinsic::Undefined)
        ConvertIntrinsicCall(ast);
    else
        ConvertFunctionCall(ast);

    VISIT_DEFAULT(CallExpr);
}

IMPLEMENT_VISIT_PROC(SwitchCase)
{
    RemoveDeadCode(ast->stmnts);

    VISIT_DEFAULT(SwitchCase);
}

/* --- Declarations --- */

IMPLEMENT_VISIT_PROC(VarDecl)
{
    RegisterDeclIdent(ast);
    VISIT_DEFAULT(VarDecl);
}

IMPLEMENT_VISIT_PROC(BufferDecl)
{
    RegisterDeclIdent(ast);
    VISIT_DEFAULT(BufferDecl);
}

IMPLEMENT_VISIT_PROC(SamplerDecl)
{
    RegisterDeclIdent(ast);
    VISIT_DEFAULT(SamplerDecl);
}

IMPLEMENT_VISIT_PROC(StructDecl)
{
    LabelAnonymousStructDecl(ast);
    RenameReservedKeyword(ast->ident);

    PushStructDecl(ast);
    OpenScope();
    {
        VISIT_DEFAULT(StructDecl);
    }
    CloseScope();
    PopStructDecl();

    if (!isVKSL_)
        RemoveSamplerStateVarDeclStmnts(ast->varMembers);

    /* Is this an empty structure? */
    if (ast->NumMemberVariables() == 0)
    {
        /* Add dummy member if the structure is empty (GLSL does not support empty structures) */
        auto dummyMember = ASTFactory::MakeVarDeclStmnt(DataType::Int, nameMangling_.temporaryPrefix + "dummy");
        ast->varMembers.push_back(dummyMember);
    }
}

/* --- Declaration statements --- */

IMPLEMENT_VISIT_PROC(FunctionDecl)
{
    PushFunctionDecl(ast);
    OpenScope();
    {
        ConvertFunctionDecl(ast);
    }
    CloseScope();
    PopFunctionDecl();
}

IMPLEMENT_VISIT_PROC(VarDeclStmnt)
{
    //TODO: move this to "TypeSpecifier" !!!
    /* Remove 'static' storage class (reserved word in GLSL) */
    ast->typeSpecifier->storageClasses.erase(StorageClass::Static);

    VISIT_DEFAULT(VarDeclStmnt);
}

IMPLEMENT_VISIT_PROC(AliasDeclStmnt)
{
    /* Add name to structure declaration, if the structure is anonymous */
    if (ast->structDecl && ast->structDecl->ident.Empty() && !ast->aliasDecls.empty())
    {
        /* Use first alias name as structure name (alias names will disappear in GLSL output) */
        ast->structDecl->ident = ast->aliasDecls.front()->ident;

        /* Update type denoters of all alias declarations */
        for (auto& aliasDecl : ast->aliasDecls)
            aliasDecl->typeDenoter->SetIdentIfAnonymous(ast->structDecl->ident);
    }

    VISIT_DEFAULT(AliasDeclStmnt);
}

/* --- Statements --- */

IMPLEMENT_VISIT_PROC(CodeBlockStmnt)
{
    bool disableNewScope = (args != nullptr ? reinterpret_cast<CodeBlockStmntArgs*>(args)->disableNewScope : false);

    if (!disableNewScope)
    {
        OpenScope();
        {
            VISIT_DEFAULT(CodeBlockStmnt);
        }
        CloseScope();
    }
    else
        VISIT_DEFAULT(CodeBlockStmnt);
}

IMPLEMENT_VISIT_PROC(ForLoopStmnt)
{
    /* Ensure a code block as body statement (if the body is a return statement within the entry point) */
    MakeCodeBlockInEntryPointReturnStmnt(ast->bodyStmnt);

    Visit(ast->attribs);
    OpenScope();
    {
        Visit(ast->initStmnt);
        Visit(ast->condition);
        Visit(ast->iteration);

        if (ast->bodyStmnt->Type() == AST::Types::CodeBlockStmnt)
        {
            /* Do NOT open a new scope for the body code block in GLSL */
            CodeBlockStmntArgs bodyStmntArgs;
            bodyStmntArgs.disableNewScope = true;
            Visit(ast->bodyStmnt, &bodyStmntArgs);
        }
        else
            Visit(ast->bodyStmnt);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(WhileLoopStmnt)
{
    /* Ensure a code block as body statement (if the body is a return statement within the entry point) */
    MakeCodeBlockInEntryPointReturnStmnt(ast->bodyStmnt);

    OpenScope();
    {
        VISIT_DEFAULT(WhileLoopStmnt);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(DoWhileLoopStmnt)
{
    /* Ensure a code block as body statement (if the body is a return statement within the entry point) */
    MakeCodeBlockInEntryPointReturnStmnt(ast->bodyStmnt);

    OpenScope();
    {
        VISIT_DEFAULT(DoWhileLoopStmnt);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(IfStmnt)
{
    /* Ensure a code block as body statement (if the body is a return statement within the entry point) */
    MakeCodeBlockInEntryPointReturnStmnt(ast->bodyStmnt);

    OpenScope();
    {
        VISIT_DEFAULT(IfStmnt);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(ElseStmnt)
{
    /* Ensure a code block as body statement (if the body is a return statement within the entry point) */
    MakeCodeBlockInEntryPointReturnStmnt(ast->bodyStmnt);

    OpenScope();
    {
        VISIT_DEFAULT(ElseStmnt);
    }
    CloseScope();
}

IMPLEMENT_VISIT_PROC(SwitchStmnt)
{
    OpenScope();
    {
        VISIT_DEFAULT(SwitchStmnt);
    }
    CloseScope();
}

/* --- Expressions --- */

//TODO:
//  move this to "ExprConverter" class,
//  and make a correct conversion of "CastExpr" for a struct-constructor (don't use list expression here)
#if 1

IMPLEMENT_VISIT_PROC(LiteralExpr)
{
    /* Replace 'h' and 'H' suffix with 'f' suffix */
    auto& s = ast->value;

    if (!s.empty())
    {
        if (s.back() == 'h' || s.back() == 'H')
        {
            s.back() = 'f';
            ast->dataType = DataType::Float;
        }
    }

    VISIT_DEFAULT(LiteralExpr);
}

IMPLEMENT_VISIT_PROC(CastExpr)
{
    /* Check if the expression must be extended for a struct c'tor */
    const auto& typeDen = ast->typeSpecifier->GetTypeDenoter()->GetAliased();
    if (auto structTypeDen = typeDen.As<StructTypeDenoter>())
    {
        if (auto structDecl = structTypeDen->structDeclRef)
        {
            /* Get the type denoter of all structure members */
            std::vector<TypeDenoterPtr> memberTypeDens;
            structDecl->CollectMemberTypeDenoters(memberTypeDens);

            /* Convert sub expression for structure c'tor */
            if (ast->expr->Type() == AST::Types::LiteralExpr)
            {
                /* Generate list expression with N copies literals (where N is the number of struct members) */
                auto literalExpr = std::static_pointer_cast<LiteralExpr>(ast->expr);
                ast->expr = ASTFactory::MakeConstructorListExpr(literalExpr, memberTypeDens);
            }
            /*else if ()
            {
                //TODO: temporary variable must be created and inserted before this expression,
                //      especially whan the sub expression contains a function call!
                //...
            }*/
        }
    }
    
    VISIT_DEFAULT(CastExpr);
}

#endif

IMPLEMENT_VISIT_PROC(ObjectExpr)
{
    if (ast->prefixExpr)
    {
        /* Convert prefix expression if it's the identifier of an entry-point struct instance */
        ConvertEntryPointStructPrefix(ast->prefixExpr, ast);
    }
    else
    {
        /* Is this object a member of the active owner structure (like 'this->memberVar')? */
        if (auto selfParam = ActiveSelfParameter())
        {
            if (auto activeStructDecl = ActiveStructDecl())
            {
                if (auto varDecl = ast->FetchVarDecl())
                {
                    if (auto structDecl = varDecl->structDeclRef)
                    {
                        if (structDecl == activeStructDecl || structDecl->IsBaseOf(*activeStructDecl))
                        {
                            /* Make the 'self'-parameter the new prefix expression */
                            ast->prefixExpr = ASTFactory::MakeObjectExpr(selfParam);
                        }
                    }
                }
            }
        }
    }
    
    VISIT_DEFAULT(ObjectExpr);
}

#undef IMPLEMENT_VISIT_PROC

/* ----- Scope functions ----- */

void GLSLConverter::OpenScope()
{
    symTable_.OpenScope();
}

void GLSLConverter::CloseScope()
{
    symTable_.CloseScope();
}

void GLSLConverter::Register(const std::string& ident)
{
    symTable_.Register(ident, true);
}

void GLSLConverter::RegisterDeclIdent(Decl* obj, bool global)
{
    /* Rename declaration object if required */
    if (MustRenameDeclIdent(obj))
        RenameDeclIdent(obj);

    /* Rename declaration object if it has a reserved keyword */
    RenameReservedKeyword(obj->ident);

    /* Register identifier in symbol table */
    if (global)
        globalReservedDecls_.push_back(obj);
    else
        Register(obj->ident);
}

void GLSLConverter::RegisterGlobalDeclIdents(const std::vector<VarDecl*>& varDecls)
{
    for (auto varDecl : varDecls)
        RegisterDeclIdent(varDecl, true);
}

bool GLSLConverter::FetchFromCurrentScope(const std::string& ident) const
{
    return symTable_.FetchFromCurrentScope(ident);
}

/* --- Helper functions for conversion --- */

bool GLSLConverter::IsSamplerStateTypeDenoter(const TypeDenoterPtr& typeDenoter) const
{
    if (typeDenoter)
    {
        if (auto samplerTypeDen = typeDenoter->GetAliased().As<SamplerTypeDenoter>())
        {
            /* Is the sampler type a sampler-state type? */
            return IsSamplerStateType(samplerTypeDen->samplerType);
        }
    }
    return false;
}

bool GLSLConverter::MustRenameDeclIdent(const Decl* obj) const
{
    if (auto varDeclObj = obj->As<VarDecl>())
    {
        /*
        Variables must be renamed if they are not inside a structure declaration and their names are reserved,
        or the identifier has already been declared in the current scope
        */
        if (InsideStructDecl() || varDeclObj->flags(VarDecl::isShaderInput))
            return false;

        /* Does the declaration object has a globally reserved identifier? */
        const auto it = std::find_if(
            globalReservedDecls_.begin(), globalReservedDecls_.end(),
            [varDeclObj](const Decl* compareObj)
            {
                return (compareObj->ident == varDeclObj->ident);
            }
        );

        if (it != globalReservedDecls_.end())
        {
            /* Is the declaration object the reserved variable? */
            return (*it != obj);
        }
    }

    /* Check if identifier has already been declared in the current scope */
    if (FetchFromCurrentScope(obj->ident))
        return true;

    return false;
}

void GLSLConverter::RenameIdent(Identifier& ident)
{
    ident.AppendPrefix(nameMangling_.temporaryPrefix);
}

void GLSLConverter::RenameDeclIdent(Decl* obj)
{
    RenameIdent(obj->ident);
}

void GLSLConverter::RenameInOutVarIdents(const std::vector<VarDecl*>& varDecls, bool input, bool useSemanticOnly)
{
    for (auto varDecl : varDecls)
    {
        if (useSemanticOnly)
            varDecl->ident = varDecl->semantic.ToString();
        else if (input)
            varDecl->ident = nameMangling_.inputPrefix + varDecl->semantic.ToString();
        else
            varDecl->ident = nameMangling_.outputPrefix + varDecl->semantic.ToString();
    }
}

void GLSLConverter::LabelAnonymousStructDecl(StructDecl* ast)
{
    if (ast->IsAnonymous())
    {
        ast->ident = nameMangling_.temporaryPrefix + "anonym" + std::to_string(anonymCounter_);
        ++anonymCounter_;
    }
}

bool GLSLConverter::IsGlobalInOutVarDecl(VarDecl* varDecl) const
{
    if (varDecl)
    {
        /* Is this variable a global input/output variable? */
        auto entryPoint = program_->entryPointRef;
        return (entryPoint->inputSemantics.Contains(varDecl) || entryPoint->outputSemantics.Contains(varDecl));
    }
    return false;
}

void GLSLConverter::MakeCodeBlockInEntryPointReturnStmnt(StmntPtr& stmnt)
{
    /* Is this statement within the entry point? */
    if (InsideEntryPoint())
    {
        if (stmnt->Type() == AST::Types::ReturnStmnt)
        {
            /* Convert statement into a code block statement */
            stmnt = ASTFactory::MakeCodeBlockStmnt(stmnt);
        }
    }
}

void GLSLConverter::RemoveDeadCode(std::vector<StmntPtr>& stmnts)
{
    for (auto it = stmnts.begin(); it != stmnts.end();)
    {
        if ((*it)->flags(AST::isDeadCode))
            it = stmnts.erase(it);
        else
            ++it;
    }
}

void GLSLConverter::RemoveSamplerStateVarDeclStmnts(std::vector<VarDeclStmntPtr>& stmnts)
{
    /* Move all variables to disabled code which are sampler state objects, since GLSL does not support sampler states */
    MoveAllIf(
        stmnts,
        program_->disabledAST,
        [&](const VarDeclStmntPtr& varDeclStmnt)
        {
            return IsSamplerStateTypeDenoter(varDeclStmnt->typeSpecifier->GetTypeDenoter());
        }
    );
}

bool GLSLConverter::RenameReservedKeyword(Identifier& ident)
{
    if (options_.obfuscate)
    {
        /* Set output identifier to an obfuscated number */
        ident = "_" + std::to_string(obfuscationCounter_++);
        return true;
    }
    else
    {
        const auto& reservedKeywords = ReservedGLSLKeywords();

        /* Perform name mangling on output identifier if the input identifier is a reserved name */
        auto it = reservedKeywords.find(ident);
        if (it != reservedKeywords.end())
        {
            ident.AppendPrefix(nameMangling_.reservedWordPrefix);
            return true;
        }

        /* Check if identifier begins with "gl_" */
        if (ident.Final().compare(0, 3, "gl_") == 0)
        {
            ident.AppendPrefix(nameMangling_.reservedWordPrefix);
            return true;
        }

        return false;
    }
}

void GLSLConverter::PushSelfParameter(VarDecl* parameter)
{
    selfParamStack_.push_back(parameter);
}

void GLSLConverter::PopSelfParameter()
{
    if (!selfParamStack_.empty())
        return selfParamStack_.pop_back();
    else
        throw std::underflow_error(R_SelfParamLevelUnderflow);
}

VarDecl* GLSLConverter::ActiveSelfParameter() const
{
    return (selfParamStack_.empty() ? nullptr : selfParamStack_.back());
}

bool GLSLConverter::CompareFuncSignatures(const FunctionDecl& lhs, const FunctionDecl& rhs)
{
    /* Compare function signatures and ignore generic sub types (GLSL has no distinction for these types) */
    return lhs.EqualsSignature(rhs, TypeDenoter::IgnoreGenericSubType);
}

/* ----- Conversion ----- */

void GLSLConverter::ConvertFunctionDecl(FunctionDecl* ast)
{
    /* Convert member function to global function */
    VarDecl* selfParamVar = nullptr;

    if (auto structDecl = ast->structDeclRef)
    {
        if (!ast->IsStatic())
        {
            /* Insert parameter of 'self' object */
            auto selfParamTypeDen   = std::make_shared<StructTypeDenoter>(structDecl);
            auto selfParamType      = ASTFactory::MakeTypeSpecifier(selfParamTypeDen);
            auto selfParam          = ASTFactory::MakeVarDeclStmnt(selfParamType, nameMangling_.namespacePrefix + "self");

            selfParam->flags << VarDeclStmnt::isSelfParameter;

            ast->parameters.insert(ast->parameters.begin(), selfParam);

            selfParamVar = selfParam->varDecls.front().get();
        }
    }

    if (selfParamVar)
        PushSelfParameter(selfParamVar);

    RenameReservedKeyword(ast->ident);

    if (ast->flags(FunctionDecl::isEntryPoint))
        ConvertFunctionDeclEntryPoint(ast);
    else
        ConvertFunctionDeclDefault(ast);

    if (!isVKSL_)
        RemoveSamplerStateVarDeclStmnts(ast->parameters);

    if (selfParamVar)
        PopSelfParameter();
}

void GLSLConverter::ConvertFunctionDeclDefault(FunctionDecl* ast)
{
    /* Default visitor */
    Visitor::VisitFunctionDecl(ast, nullptr);
}

void GLSLConverter::ConvertFunctionDeclEntryPoint(FunctionDecl* ast)
{
    /* Propagate array parameter declaration to input/output semantics */
    for (auto param : ast->parameters)
    {
        if (!param->varDecls.empty())
        {
            auto varDecl = param->varDecls.front();
            const auto& typeDen = varDecl->GetTypeDenoter()->GetAliased();
            if (auto arrayTypeDen = typeDen.As<ArrayTypeDenoter>())
            {
                /* Mark this member and all structure members as dynamic array */
                varDecl->flags << VarDecl::isDynamicArray;

                const auto& subTypeDen = arrayTypeDen->subTypeDenoter->GetAliased();
                if (auto structSubTypeDen = subTypeDen.As<StructTypeDenoter>())
                {
                    if (structSubTypeDen->structDeclRef)
                    {
                        structSubTypeDen->structDeclRef->ForEachVarDecl(
                            [](VarDeclPtr& member)
                            {
                                member->flags << VarDecl::isDynamicArray;
                            }
                        );
                    }
                }
            }
        }
    }

    /* Default visitor */
    Visitor::VisitFunctionDecl(ast, nullptr);
}

void GLSLConverter::ConvertIntrinsicCall(CallExpr* ast)
{
    switch (ast->intrinsic)
    {
        case Intrinsic::Saturate:
            ConvertIntrinsicCallSaturate(ast);
            break;
        case Intrinsic::Texture_Sample_2:
        case Intrinsic::Texture_Sample_3:
        case Intrinsic::Texture_Sample_4:
        case Intrinsic::Texture_Sample_5:
            ConvertIntrinsicCallTextureSample(ast);
            break;
        case Intrinsic::Texture_SampleLevel_3:
        case Intrinsic::Texture_SampleLevel_4:
        case Intrinsic::Texture_SampleLevel_5:
            ConvertIntrinsicCallTextureSampleLevel(ast);
            break;
        case Intrinsic::InterlockedAdd:
        case Intrinsic::InterlockedAnd:
        case Intrinsic::InterlockedOr:
        case Intrinsic::InterlockedXor:
        case Intrinsic::InterlockedMin:
        case Intrinsic::InterlockedMax:
        case Intrinsic::InterlockedCompareExchange:
        case Intrinsic::InterlockedExchange:
            ConvertIntrinsicCallImageAtomic(ast);
            break;
        default:
            break;
    }
}

void GLSLConverter::ConvertIntrinsicCallSaturate(CallExpr* ast)
{
    /* Convert "saturate(x)" to "clamp(x, genType(0), genType(1))" */
    if (ast->arguments.size() == 1)
    {
        auto argTypeDen = ast->arguments.front()->GetTypeDenoter()->GetSub();
        if (argTypeDen->IsBase())
        {
            ast->intrinsic = Intrinsic::Clamp;
            ast->arguments.push_back(ASTFactory::MakeLiteralCastExpr(argTypeDen, DataType::Int, "0"));
            ast->arguments.push_back(ASTFactory::MakeLiteralCastExpr(argTypeDen, DataType::Int, "1"));
        }
        else
            RuntimeErr(R_InvalidIntrinsicArgType("saturate"), ast->arguments.front().get());
    }
    else
        RuntimeErr(R_InvalidIntrinsicArgCount("saturate"), ast);
}

static int GetTextureVectorSizeFromIntrinsicCall(CallExpr* ast)
{
    /* Get buffer object from sample intrinsic call */
    if (const auto& prefixExpr = ast->prefixExpr)
    {
        if (auto lvalueExpr = ast->prefixExpr->FetchLValueExpr())
        {
            if (auto bufferDecl = lvalueExpr->FetchSymbol<BufferDecl>())
            {
                /* Determine vector size for texture intrinsic parametes */
                switch (bufferDecl->GetBufferType())
                {
                    case BufferType::Texture1D:
                        return 1;
                    case BufferType::Texture1DArray:
                    case BufferType::Texture2D:
                    case BufferType::Texture2DMS:
                        return 2;
                    case BufferType::Texture2DArray:
                    case BufferType::Texture2DMSArray:
                    case BufferType::Texture3D:
                    case BufferType::TextureCube:
                        return 3;
                    case BufferType::TextureCubeArray:
                        return 4;
                    default:
                        break;
                }
            }
        }
    }
    return 0;
}

void GLSLConverter::ConvertIntrinsicCallTextureSample(CallExpr* ast)
{
    /* Determine vector size for texture intrinsic */
    if (auto vectorSize = GetTextureVectorSizeFromIntrinsicCall(ast))
    {
        /* Convert arguments */
        auto& args = ast->arguments;

        /* Ensure argument: float[1,2,3,4] Location */
        if (args.size() >= 2)
            exprConverter_.ConvertExprIfCastRequired(args[1], VectorDataType(DataType::Float, vectorSize), true);

        /* Ensure argument: int[1,2,3] Offset */
        if (args.size() >= 3)
            exprConverter_.ConvertExprIfCastRequired(args[2], VectorDataType(DataType::Int, vectorSize), true);
    }
}

void GLSLConverter::ConvertIntrinsicCallTextureSampleLevel(CallExpr* ast)
{
    /* Determine vector size for texture intrinsic */
    if (auto vectorSize = GetTextureVectorSizeFromIntrinsicCall(ast))
    {
        /* Convert arguments */
        auto& args = ast->arguments;

        /* Ensure argument: float[1,2,3,4] Location */
        if (args.size() >= 2)
            exprConverter_.ConvertExprIfCastRequired(args[1], VectorDataType(DataType::Float, vectorSize), true);

        /* Ensure argument: int[1,2,3] Offset */
        if (args.size() >= 4)
            exprConverter_.ConvertExprIfCastRequired(args[3], VectorDataType(DataType::Int, vectorSize), true);
    }
}

void GLSLConverter::ConvertIntrinsicCallImageAtomic(CallExpr* ast)
{
    /* Convert "atomic*" to "imageAtomic*" for buffer types */
    if (ast->arguments.size() >= 2)
    {
        const auto& arg0Expr = ast->arguments.front();
        if (auto arg0ArrayExpr = arg0Expr->As<ArrayExpr>())
        {
            const auto& typeDen = arg0ArrayExpr->prefixExpr->GetTypeDenoter()->GetAliased();
            if (auto bufferTypeDen = typeDen.As<BufferTypeDenoter>())
            {
                /* Is the buffer declaration a read/write texture? */
                if (IsRWTextureBufferType(bufferTypeDen->bufferType))
                {
                    /* Map interlocked intrinsic to image atomic intrinsic */
                    ast->intrinsic = InterlockedToImageAtomicIntrinsic(ast->intrinsic);

                    /* Insert array indices from object identifier after first argument */
                    ast->arguments.insert(ast->arguments.begin() + 1, arg0ArrayExpr->arrayIndices.back());

                    /* Check if array expression must be replaced by its sub expression */
                    arg0ArrayExpr->arrayIndices.pop_back();
                    if (arg0ArrayExpr->arrayIndices.empty())
                        ast->arguments.front() = arg0ArrayExpr->prefixExpr;
                }
            }
        }
        else
        {
            const auto& typeDen = arg0Expr->GetTypeDenoter()->GetAliased();
            if (auto bufferTypeDen = typeDen.As<BufferTypeDenoter>())
            {
                /* Is the buffer declaration a read/write texture? */
                if (IsRWTextureBufferType(bufferTypeDen->bufferType))
                {
                    /* Map interlocked intrinsic to image atomic intrinsic */
                    ast->intrinsic = InterlockedToImageAtomicIntrinsic(ast->intrinsic);
                }
            }
        }
    }
}

void GLSLConverter::ConvertFunctionCall(CallExpr* ast)
{
    if (auto funcDecl = ast->funcDeclRef)
    {
        if (funcDecl->IsMemberFunction())
        {
            if (funcDecl->IsStatic())
            {
                /* Drop prefix expression, since GLSL only allows global functions */
                ast->prefixExpr.reset();
            }
            else
            {
                if (ast->prefixExpr)
                {
                    /* Move prefix expression as argument into the function call */
                    ast->PushArgumentFront(std::move(ast->prefixExpr));
                }
                else
                {
                    if (auto selfParam = ActiveSelfParameter())
                    {
                        /* Insert current 'self'-parameter as argument into the function call */
                        ast->PushArgumentFront(ASTFactory::MakeObjectExpr(selfParam));
                    }
                    else
                        RuntimeErr(R_MissingSelfParamForMemberFunc(funcDecl->ToString()), ast);
                }
            }
        }
    }
}

/*
~~~~~~~~~~~~~~~ TODO: refactor this ~~~~~~~~~~~~~~~
*/
void GLSLConverter::ConvertEntryPointStructPrefix(ExprPtr& expr, ObjectExpr* objectExpr)
{
    auto nonBracketExpr = expr->FetchNonBracketExpr();
    if (auto prefixExpr = nonBracketExpr->As<ObjectExpr>())
        ConvertEntryPointStructPrefixObject(expr, prefixExpr, objectExpr);
    else if (auto prefixExpr = nonBracketExpr->As<ArrayExpr>())
        ConvertEntryPointStructPrefixArray(expr, prefixExpr, objectExpr);
}

// Marks the object expression as 'immutable', if the specified structure is a non-entry-point (NEP) parameter
static bool MakeObjectExprImmutableForNEPStruct(ObjectExpr* objectExpr, const StructDecl* structDecl)
{
    if (structDecl)
    {
        if (structDecl->flags(StructDecl::isNonEntryPointParam))
        {
            /* Mark object expression as immutable */
            objectExpr->flags << ObjectExpr::isImmutable;
            return true;
        }
    }
    return false;
}

void GLSLConverter::ConvertEntryPointStructPrefixObject(ExprPtr& expr, ObjectExpr* prefixExpr, ObjectExpr* objectExpr)
{
    /* Does this l-value refer to a variable declaration? */
    if (auto varDecl = prefixExpr->FetchVarDecl())
    {
        /* Is its type denoter a structure? */
        const auto& varTypeDen = varDecl->GetTypeDenoter()->GetAliased();
        if (auto structTypeDen = varTypeDen.As<StructTypeDenoter>())
        {
            /* Can the structure be resolved? */
            if (!MakeObjectExprImmutableForNEPStruct(objectExpr, structTypeDen->structDeclRef))
            {
                /* Drop prefix expression for global input/output variables */
                if (IsGlobalInOutVarDecl(objectExpr->FetchVarDecl()))
                    expr.reset();
            }
        }
    }
}

void GLSLConverter::ConvertEntryPointStructPrefixArray(ExprPtr& expr, ArrayExpr* prefixExpr, ObjectExpr* objectExpr)
{
    /* Does this l-value refer to a variable declaration? */
    if (auto varDecl = prefixExpr->prefixExpr->FetchVarDecl())
    {
        /* Is its type denoter an array of structures? */
        const auto& varTypeDen = varDecl->GetTypeDenoter()->GetAliased();
        if (auto arrayTypeDen = varTypeDen.As<ArrayTypeDenoter>())
        {
            const auto& varSubTypeDen = arrayTypeDen->subTypeDenoter->GetAliased();
            if (auto structTypeDen = varSubTypeDen.As<StructTypeDenoter>())
            {
                /* Can the structure be resolved? */
                MakeObjectExprImmutableForNEPStruct(objectExpr, structTypeDen->structDeclRef);
            }
        }
    }
}

/* ----- Unrolling ----- */

void GLSLConverter::UnrollStmnts(std::vector<StmntPtr>& stmnts)
{
    for (auto it = stmnts.begin(); it != stmnts.end();)
    {
        std::vector<StmntPtr> unrolledStmnts;

        auto ast = it->get();
        if (auto varDeclStmnt = ast->As<VarDeclStmnt>())
        {
            if (options_.unrollArrayInitializers)
                UnrollStmntsVarDecl(unrolledStmnts, varDeclStmnt);
        }

        ++it;

        if (!unrolledStmnts.empty())
        {
            it = stmnts.insert(it, unrolledStmnts.begin(), unrolledStmnts.end());
            it += unrolledStmnts.size();
        }
    }
}

void GLSLConverter::UnrollStmntsVarDecl(std::vector<StmntPtr>& unrolledStmnts, VarDeclStmnt* ast)
{
    /* Unroll all array initializers */
    for (const auto& varDecl : ast->varDecls)
    {
        if (varDecl->initializer)
            UnrollStmntsVarDeclInitializer(unrolledStmnts, varDecl.get());
    }
}

void GLSLConverter::UnrollStmntsVarDeclInitializer(std::vector<StmntPtr>& unrolledStmnts, VarDecl* varDecl)
{
    const auto& typeDen = varDecl->GetTypeDenoter()->GetAliased();
    if (auto arrayTypeDen = typeDen.As<ArrayTypeDenoter>())
    {
        /* Get initializer expression */
        if (auto initExpr = varDecl->initializer->As<InitializerExpr>())
        {
            /* Get dimension sizes of array type denoter */
            auto dimSizes = arrayTypeDen->GetDimensionSizes();
            std::vector<int> arrayIndices(dimSizes.size(), 0);

            /* Generate array element assignments until no further array index can be fetched */
            do
            {
                /* Fetch sub expression from initializer */
                auto subExpr = initExpr->FetchSubExpr(arrayIndices);

                /* Make new statement for current array element assignment */
                auto assignStmnt = ASTFactory::MakeArrayAssignStmnt(varDecl, arrayIndices, subExpr);

                /* Append new statement to list */
                unrolledStmnts.push_back(assignStmnt);
            }
            while (initExpr->NextArrayIndices(arrayIndices));

            /* Remove initializer after unrolling */
            varDecl->initializer.reset();
        }
    }
}


} // /namespace Xsc



// ================================================================================
