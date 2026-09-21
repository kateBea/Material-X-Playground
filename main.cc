#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>

#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>

#include <MaterialXGenGlsl/GlslShaderGenerator.h>

#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/ShaderGenerator.h>
#include <MaterialXGenShader/ShaderStage.h>

#include <MaterialXGenSlang/SlangShaderGenerator.h>

#include <slang/slang-com-ptr.h>
#include <slang/slang.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mx = MaterialX;
namespace fs = std::filesystem;

namespace {

    auto WriteTextFile( const fs::path &path, const std::string &contents ) -> void {
        std::ofstream file{ path, std::ios::out | std::ios::binary | std::ios::trunc };

        if ( !file ) {
            throw std::runtime_error{ "Failed to open output file: " + path.string() };
        }

        file.write( contents.data(), static_cast<std::streamsize>( contents.size() ) );
    }

    auto GetSlangDiagnostics( const Slang::ComPtr<slang::IBlob> &diagnostics )
            -> std::string {
        if ( !diagnostics ) {
            return {};
        }

        const auto *data{ static_cast<const char *>( diagnostics->getBufferPointer() ) };
        const auto size{ diagnostics->getBufferSize() };

        return std::string{ data, size };
    }

    auto CheckSlang( SlangResult result, std::string_view operation,
                     const Slang::ComPtr<slang::IBlob> &diagnostics = nullptr )
            -> void {
        if ( SLANG_SUCCEEDED( result ) ) {
            return;
        }

        std::string message{ "Slang operation failed: " };
        message += operation;

        const std::string diagnosticText{ GetSlangDiagnostics( diagnostics ) };

        if ( !diagnosticText.empty() ) {
            message += "\n\n";
            message += diagnosticText;
        }

        throw std::runtime_error{ message };
    }

    auto GenerateShader( const mx::ShaderGeneratorPtr &generator,
                         const mx::DocumentPtr &standardLibrary,
                         const mx::NodePtr &material,
                         const mx::FileSearchPath &sourceSearchPath )
            -> mx::ShaderPtr {
        generator->registerTypeDefs( standardLibrary );

        mx::GenContext context{ generator };
        context.registerSourceCodeSearchPath( sourceSearchPath );

        mx::ShaderPtr shader{
            generator->generate( material->getName(), material, context )
        };

        if ( !shader ) {
            throw std::runtime_error{ "Shader generation failed for material: " +
                                      material->getName() };
        }

        return shader;
    }

    auto FindMaterial( const mx::DocumentPtr &document,
                       const std::string &requestedName ) -> mx::NodePtr {
        const std::vector<mx::NodePtr> materials{ document->getMaterialNodes() };

        if ( materials.empty() ) {
            throw std::runtime_error{
                "The MaterialX document contains no material nodes."
            };
        }

        if ( requestedName.empty() ) {
            return materials.front();
        }

        for ( const mx::NodePtr &material: materials ) {
            if ( material->getName() == requestedName ) {
                return material;
            }
        }

        std::string message{ "Could not find material '" };
        message += requestedName;
        message += "'. Available materials:";

        for ( const mx::NodePtr &material: materials ) {
            message += "\n    ";
            message += material->getName();
        }

        throw std::runtime_error{ message };
    }

    auto TranspileSlangToHlsl( slang::IGlobalSession *globalSession,
                               const std::string &source,
                               const std::string &moduleName,
                               const std::string &entryPointName ) -> std::string {
        slang::TargetDesc targetDescription{};
        targetDescription.format = SLANG_HLSL;
        targetDescription.profile = globalSession->findProfile( "sm_6_5" );

        slang::SessionDesc sessionDescription{};
        sessionDescription.targets = &targetDescription;
        sessionDescription.targetCount = 1;

        Slang::ComPtr<slang::ISession> session{};

        CheckSlang(
                globalSession->createSession( sessionDescription, session.writeRef() ),
                "create HLSL session" );

        Slang::ComPtr<slang::IBlob> diagnostics{};

        const std::string virtualPath{ moduleName + ".slang" };

        Slang::ComPtr<slang::IModule> module{ session->loadModuleFromSourceString(
                moduleName.c_str(), virtualPath.c_str(), source.c_str(),
                diagnostics.writeRef() ) };

        if ( !module ) {
            std::string message{ "Failed to load generated MaterialX Slang module." };

            const std::string diagnosticText{ GetSlangDiagnostics( diagnostics ) };

            if ( !diagnosticText.empty() ) {
                message += "\n\n";
                message += diagnosticText;
            }

            throw std::runtime_error{ message };
        }

        Slang::ComPtr<slang::IEntryPoint> entryPoint{};

        diagnostics.setNull();

        CheckSlang( module->findEntryPointByName( entryPointName.c_str(),
                                                  entryPoint.writeRef() ),
                    "find entry point '" + entryPointName + "'", diagnostics );

        slang::IComponentType *components[]{ module.get(), entryPoint.get() };

        Slang::ComPtr<slang::IComponentType> composedProgram{};

        diagnostics.setNull();

        CheckSlang( session->createCompositeComponentType(
                            components, static_cast<SlangInt>( std::size( components ) ),
                            composedProgram.writeRef(), diagnostics.writeRef() ),
                    "create composite program", diagnostics );

        Slang::ComPtr<slang::IComponentType> linkedProgram{};

        diagnostics.setNull();

        CheckSlang(
                composedProgram->link( linkedProgram.writeRef(), diagnostics.writeRef() ),
                "link program", diagnostics );

        Slang::ComPtr<slang::IBlob> hlslCode{};

        diagnostics.setNull();

        CheckSlang( linkedProgram->getEntryPointCode( 0, 0, hlslCode.writeRef(),
                                                      diagnostics.writeRef() ),
                    "generate HLSL", diagnostics );

        const auto *data{ static_cast<const char *>( hlslCode->getBufferPointer() ) };
        const auto size{ hlslCode->getBufferSize() };

        return std::string{ data, size };
    }

    auto WriteGeneratedStage( const mx::ShaderPtr &shader,
                              const std::string &stageName,
                              const fs::path &outputPath ) -> void {
        if ( !shader->hasStage( stageName ) ) {
            return;
        }

        WriteTextFile( outputPath, shader->getSourceCode( stageName ) );
        std::cout << "Wrote " << outputPath.string() << '\n';
    }

    auto WriteHlslStage( slang::IGlobalSession *slangSession,
                         const mx::ShaderPtr &slangShader,
                         const std::string &stageName, const fs::path &outputPath,
                         const std::string &moduleName ) -> void {
        if ( !slangShader->hasStage( stageName ) ) {
            return;
        }

        const mx::ShaderStage &stage{ slangShader->getStage( stageName ) };

        const std::string hlsl{ TranspileSlangToHlsl( slangSession,
                                                      stage.getSourceCode(), moduleName,
                                                      stage.getFunctionName() ) };

        WriteTextFile( outputPath, hlsl );
        std::cout << "Wrote " << outputPath.string() << '\n';
    }

}// namespace

auto main( int argc, char **argv ) -> int {
    try {
        if ( argc < 2 ) {
            std::cerr << "Usage:\n"
                      << "    MaterialXCodegen <material.mtlx> [material-name] "
                         "[output-directory]\n\n"
                      << "Examples:\n"
                      << "    MaterialXCodegen material.mtlx\n"
                      << "    MaterialXCodegen material.mtlx MyMaterial\n"
                      << "    MaterialXCodegen material.mtlx MyMaterial generated\n";

            return 1;
        }

        const fs::path inputPath{ fs::absolute( argv[1] ) };
        const std::string requestedMaterialName{ argc >= 3 ? argv[2] : "" };
        const fs::path outputDirectory{ argc >= 4 ? fs::path{ argv[3] }
                                                  : fs::path{ "generated" } };

        if ( !fs::exists( inputPath ) ) {
            throw std::runtime_error{ "Input MaterialX file does not exist: " +
                                      inputPath.string() };
        }

        fs::create_directories( outputDirectory );

        // ====================================================================
        // MaterialX library paths
        // ====================================================================

        const mx::FilePath materialXRoot{ MATERIALX_SOURCE_ROOT };

        mx::FileSearchPath materialXLibrarySearchPath{};
        materialXLibrarySearchPath.append( materialXRoot );

        // ====================================================================
        // Load standard MaterialX libraries
        // ====================================================================

        mx::DocumentPtr standardLibrary{ mx::createDocument() };

        const mx::FilePathVec libraryFolders{ mx::FilePath{ "libraries" } };
        const mx::StringSet loadedLibraries{ mx::loadLibraries(
                libraryFolders, materialXLibrarySearchPath, standardLibrary ) };

        if ( loadedLibraries.empty() ) {
            throw std::runtime_error{
                "Failed to load the MaterialX standard libraries from: " +
                materialXRoot.asString()
            };
        }

        std::cout << "Loaded " << loadedLibraries.size()
                  << " MaterialX library files.\n";

        // ====================================================================
        // Load requested MaterialX document
        // ====================================================================

        mx::DocumentPtr document{ mx::createDocument() };

        mx::FileSearchPath documentSearchPath{};

        if ( inputPath.has_parent_path() ) {
            documentSearchPath.append( mx::FilePath{ inputPath.parent_path().string() } );
        }

        documentSearchPath.append( materialXRoot );

        mx::readFromXmlFile( document, mx::FilePath{ inputPath.string() },
                             documentSearchPath );

        document->setDataLibrary( standardLibrary );

        // ====================================================================
        // Validate
        // ====================================================================

        std::string validationMessage{};

        if ( !document->validate( &validationMessage ) ) {
            throw std::runtime_error{ "MaterialX document validation failed:\n" +
                                      validationMessage };
        }

        std::cout << "MaterialX document is valid.\n";

        // ====================================================================
        // Select material
        // ====================================================================

        mx::NodePtr material{ FindMaterial( document, requestedMaterialName ) };

        std::cout << "Generating shaders for material: " << material->getName()
                  << '\n';

        // ====================================================================
        // Shader source search path
        //
        // MaterialX library sources + any custom implementations beside the
        // input material.
        // ====================================================================

        mx::FileSearchPath shaderSourceSearchPath{};
        shaderSourceSearchPath.append( materialXRoot );

        if ( inputPath.has_parent_path() ) {
            shaderSourceSearchPath.append(
                    mx::FilePath{ inputPath.parent_path().string() } );
        }

        // ====================================================================
        // Generate GLSL directly from MaterialX
        // ====================================================================

        mx::ShaderGeneratorPtr glslGenerator{ mx::GlslShaderGenerator::create() };

        mx::ShaderPtr glslShader{ GenerateShader( glslGenerator, standardLibrary,
                                                  material, shaderSourceSearchPath ) };

        // ====================================================================
        // Generate Slang directly from MaterialX
        // ====================================================================

        mx::ShaderGeneratorPtr slangGenerator{ mx::SlangShaderGenerator::create() };

        mx::ShaderPtr slangShader{ GenerateShader( slangGenerator, standardLibrary,
                                                   material, shaderSourceSearchPath ) };

        // ====================================================================
        // Output filenames
        // ====================================================================

        const std::string baseName{ material->getName() };

        const fs::path glslVertexPath{ outputDirectory / ( baseName + ".vert.glsl" ) };
        const fs::path glslFragmentPath{ outputDirectory /
                                         ( baseName + ".frag.glsl" ) };
        const fs::path slangVertexPath{ outputDirectory /
                                        ( baseName + ".vert.slang" ) };
        const fs::path slangFragmentPath{ outputDirectory /
                                          ( baseName + ".frag.slang" ) };
        const fs::path hlslVertexPath{ outputDirectory / ( baseName + ".vert.hlsl" ) };
        const fs::path hlslFragmentPath{ outputDirectory /
                                         ( baseName + ".frag.hlsl" ) };

        // ====================================================================
        // Write GLSL
        // ====================================================================

        WriteGeneratedStage( glslShader, mx::Stage::VERTEX, glslVertexPath );
        WriteGeneratedStage( glslShader, mx::Stage::PIXEL, glslFragmentPath );

        // ====================================================================
        // Write Slang
        // ====================================================================

        WriteGeneratedStage( slangShader, mx::Stage::VERTEX, slangVertexPath );
        WriteGeneratedStage( slangShader, mx::Stage::PIXEL, slangFragmentPath );

        // ====================================================================
        // Slang compiler
        // ====================================================================

        Slang::ComPtr<slang::IGlobalSession> globalSlangSession{};
        CheckSlang( slang::createGlobalSession( globalSlangSession.writeRef() ),
                    "create global Slang session" );

        // ====================================================================
        // Slang to HLSL
        // ====================================================================

        WriteHlslStage( globalSlangSession, slangShader, mx::Stage::VERTEX,
                        hlslVertexPath, baseName + "_vertex" );
        WriteHlslStage( globalSlangSession, slangShader, mx::Stage::PIXEL,
                        hlslFragmentPath, baseName + "_fragment" );

        std::cout << "\nDone.\n\n"
                  << "Output directory: " << fs::absolute( outputDirectory ).string()
                  << "\n\n"
                  << "Generated:\n"
                  << "    " << glslVertexPath.filename().string() << '\n'
                  << "    " << glslFragmentPath.filename().string() << '\n'
                  << "    " << slangVertexPath.filename().string() << '\n'
                  << "    " << slangFragmentPath.filename().string() << '\n'
                  << "    " << hlslVertexPath.filename().string() << '\n'
                  << "    " << hlslFragmentPath.filename().string() << '\n';

        return 0;
    } catch ( const std::exception &exception ) {
        std::cerr << "\nERROR:\n"
                  << exception.what() << '\n';

        return 1;
    }
}
