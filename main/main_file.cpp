#include <stdio.h>
#include <assert.h>
#include "Binary.h"
#include "binary/elf/ElfBinaryAnalyzer.h"
#include "binary/ihex/IHexBinaryAnalyzer.h"
#include "arch/x86/X86FunctionAnalyzer.h"
#include "arch/AvrFunctionAnalyzer.h"

#include "Main.h"
#include "FileFormat.h"
#include "Architecture.h"
#include "IRGen.h"
#include "SSAGen.h"
#include "SSAPhiNodeGenerator.h"
#include "SSAAddressToBlockTransformer.h"
#include "SSACallingConvApplier.h"
#include "SSADCETransformer.h"
#include "SSACalleeCallerRegs.h"
#include "HIdList.h"
#include "SSAPeepholeOptimizer.h"
#include "SSATransformToC.h"
#include "SSAAppendSimplifier.h"
#include "SSAApplyRegRef.h"
#include "PeepholeOptimizer.h"
#include "ScriptingInterface.h"

#include "CHolodec.h"

#include <thread>

#include "HoloIO.h"


//FileFormat		-> proposes BinaryAnalyzer
//BinaryAnalyzer	-> configuration
//					-> proposes Architecture
//					-> maps data into vmemory
//Architecture		-> proposes FunctionAnalyzer

//Binary -> Symbol, Section, Function, DataSegment, Architecture*
//Binary -> Architecture*
//Symbol -> Symbol, Binary*
//Section -> Symbol, Binary*, CC*
//Function -> Symbol, Binary*, SSARep
//Architecture -> CC, ISA, Regs

using namespace holodec;

FileFormat elffileformat = { "elf", "elf",{
	[](File* file, HString name) {
		static holoelf::ElfBinaryAnalyzer* analyzer = nullptr;
		if (analyzer == nullptr) {
			printf("Create New Object\n");
			analyzer = new holoelf::ElfBinaryAnalyzer();
		}
		if (analyzer->canAnalyze(file)) {
			holoelf::ElfBinaryAnalyzer* temp = analyzer;
			analyzer = nullptr;
			return (BinaryAnalyzer*)temp;
		}
		return (BinaryAnalyzer*) nullptr;
	}
}
};
FileFormat ihexfileformat = { "ihex", "ihex",{
	[](File* file, HString name) {
		static holoihex::IHexBinaryAnalyzer* analyzer = nullptr;
		if (analyzer == nullptr) {
			printf("Create New Object\n");
			analyzer = new holoihex::IHexBinaryAnalyzer();
		}
		if (analyzer->canAnalyze(file)) {
			holoihex::IHexBinaryAnalyzer* temp = analyzer;
			analyzer = nullptr;
			return (BinaryAnalyzer*)temp;
		}
		return (BinaryAnalyzer*) nullptr;
	}
}
};
extern Architecture holox86::x86architecture;


int main (int argc, const char** argv) {

	g_logger.log<LogLevel::eInfo> ("Init X86\n");

	if (argc < 2) {
		g_logger.log<LogLevel::eWarn>("No parameters given\n");
		return -1;
	}

	g_logger.log<LogLevel::eInfo>("Analysing file %s\n", argv[1]);
	HString filename = argv[1];
	Main::initMain();
	File* file = Main::loadDataFromFile (filename);
	if (!file) {
		g_logger.log<LogLevel::eWarn> ("Could not Load File %s\n", filename.cstr());
		return -1;
	}

	Main::g_main->registerFileFormat (&elffileformat);
	Main::g_main->registerArchitecture (&holox86::x86architecture);

	Main::g_main->registerFileFormat(&ihexfileformat);
	Main::g_main->registerArchitecture(&holoavr::avrarchitecture);

	g_logger.log<LogLevel::eInfo> ("Init X86\n");
	holox86::x86architecture.init();
	holoavr::avrarchitecture.init();

	//ScriptingInterface script;
	//script.testModule(&holox86::x86architecture);


	BinaryAnalyzer* analyzer = nullptr;
	for (FileFormat * fileformat : Main::g_main->fileformats) {
		analyzer = fileformat->createBinaryAnalyzer (file, "binary");
		if (analyzer)
			break;
	}
	analyzer->init (file);
	Binary* binary = analyzer->binary;

	for(Section* section : binary->sections){
		section->print();
	}

	FunctionAnalyzer* func_analyzer = nullptr;
	for (Architecture * architecture : Main::g_main->architectures) {
		func_analyzer = architecture->createFunctionAnalyzer (binary);
		if (func_analyzer)
			break;
	}
	assert(func_analyzer);
	func_analyzer->init (binary);

	printf ("DataSegment: %s\n", binary->defaultArea->name.name.cstr());

	binary->print();

	for (Symbol* sym : binary->symbols) {
		if (sym->symboltype == &SymbolType::symfunc) {
			Function* newfunction = new Function();
			newfunction->symbolref = sym->id;
			newfunction->baseaddr = sym->vaddr;
			newfunction->addrToAnalyze.insert (sym->vaddr);
			binary->functions.push_back (newfunction);
		}
	}
	bool funcAnalyzed;
	do {
		funcAnalyzed = false;
		for (Function* func : binary->functions) {
			if (!func->addrToAnalyze.empty()) {
				func_analyzer->analyzeFunction (func);
				funcAnalyzed = true;
				if (!func->funcsCalled.empty()) {
					for (uint64_t addr : func->funcsCalled) {
						if (binary->findSymbol (addr, &SymbolType::symfunc) == nullptr) {
							char buffer[100];
							snprintf (buffer, 100, "func_0x%" PRIx64 "", addr);
							Symbol* symbol = new Symbol ({0, buffer, &SymbolType::symfunc, 0, addr, 0});
							binary->addSymbol (symbol);
							Function* newfunction = new Function();
							newfunction->symbolref = symbol->id;
							newfunction->baseaddr = symbol->vaddr;
							newfunction->addrToAnalyze.insert(symbol->vaddr);
							binary->functions.push_back (newfunction);
						}
					}
				}
				break;
			}
		}
	} while (funcAnalyzed);

	binary->print();

	std::vector<StringRef> volatileRegisters = { "cf" , "zf" , "nf" , "vf" , "sf" , "hf" , "tf" , "if" };

	std::vector<SSATransformer*> transformers = {
		new SSAAddressToBlockTransformer(),//0
		new SSAPhiNodeGenerator(),//1
		new SSAPeepholeOptimizer(),//2
		new SSADCETransformer(),//3
		new SSAApplyRegRef(),//4
		new SSAAppendSimplifier(),//5
		new SSACalleeCallerRegs(volatileRegisters),//6
		new SSATransformToC(),//7
	};

	for (SSATransformer* transform : transformers) {
		if(transform)
			transform->arch = binary->arch;
	}

	PeepholeOptimizer* optimizer = parsePhOptimizer ();

	g_peephole_logger.level = LogLevel::eDebug;
	for (Function* func : binary->functions) {
		printf("Function: %s\n", binary->getSymbol(func->symbolref)->name.cstr());
	}
	g_peephole_logger.level = LogLevel::eInfo;
	
	HSet<uint64_t> funcs = {
		0x0,
	};
	for (Function* func : binary->functions) {
	//for (uint64_t addr : funcs) {
	//	Function* func = binary->getFunctionByAddr(addr);
		if (func) {
			transformers[0]->doTransformation(binary, func);
			transformers[1]->doTransformation(binary, func);
			/*if (!func->ssaRep.checkIntegrity()) {
				func->print(binary->arch);
				assert(false);
			}*/
			func->ssaRep.recalcRefCounts();
		}
	}
	bool funcChanged = false;
	do {
		printf("---------------------\n");
		printf("Run Transformations\n");
		printf("---------------------\n");
		funcChanged = false;

		for (Function* func : binary->functions) {
		//for (uint64_t addr : funcs) {
		//	Function* func = binary->getFunctionByAddr(addr);
			if (func) {
				bool applied = false;
				/*if (!func->ssaRep.checkIntegrity()) {
					func->print(binary->arch);
					assert(false);
				}*/
				do {
					applied = false;
					func->ssaRep.recalcRefCounts();
					/*if (!func->ssaRep.checkIntegrity()) {
						func->print(binary->arch);
						assert(false);
					}*/
					if (func->baseaddr == 0x0)
						func->printSimple(binary->arch);
					applied |= transformers[2]->doTransformation(binary, func);
					applied |= transformers[3]->doTransformation(binary, func);
					applied |= transformers[4]->doTransformation(binary, func);
					applied |= transformers[5]->doTransformation(binary, func);
					funcChanged |= transformers[6]->doTransformation(binary, func);
					funcChanged |= applied;
				} while (applied);
				func->ssaRep.recalcRefCounts();
			}
		}
	} while (funcChanged);
	for (Function* func : binary->functions) {
		func->print(binary->arch);
	}
	for (Function* func : binary->functions) {
	//for (uint64_t addr : funcs) {
	//	Function* func = binary->getFunctionByAddr(addr);
		if (func) {
			func->ssaRep.recalcRefCounts();
			holodec::g_logger.log<LogLevel::eInfo>("Symbol %s", binary->getSymbol(func->symbolref)->name.cstr());
			transformers[7]->doTransformation(binary, func);
		}
	}

	Function* func = new Function();
	for (int i = 0; i < 8; i++) {
		func->ssaRep.bbs.emplace_back();
	}

#define PATH(function, from, to) function->ssaRep.bbs[from].outBlocks.insert(to);function->ssaRep.bbs[to].inBlocks.insert(from);

	SSAExpression retexpr;
	retexpr.type = SSAExprType::eReturn;
	SSAExpression expr;
	expr.type = SSAExprType::eBranch;
	PATH(func, 1, 2);
	PATH(func, 2, 3);
	PATH(func, 2, 4);
	PATH(func, 3, 4);
	PATH(func, 3, 5);
	PATH(func, 4, 3);
	PATH(func, 4, 6);
	PATH(func, 5, 7);
	PATH(func, 5, 8);
	PATH(func, 6, 7);
	PATH(func, 7, 8);
	expr.subExpressions = { SSAArgument::createBlock(2) };
	func->ssaRep.bbs[1].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(3), SSAArgument::createUVal(0, 8), SSAArgument::createBlock(4) };
	func->ssaRep.bbs[2].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(4), SSAArgument::createUVal(0, 8), SSAArgument::createBlock(5) };
	func->ssaRep.bbs[3].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(3), SSAArgument::createUVal(0, 8), SSAArgument::createBlock(6) };
	func->ssaRep.bbs[4].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(7), SSAArgument::createUVal(0, 8), SSAArgument::createBlock(8) };
	func->ssaRep.bbs[5].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(7) };
	func->ssaRep.bbs[6].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(8) };
	func->ssaRep.bbs[7].exprIds.push_back(func->ssaRep.addExpr(&expr));
	func->ssaRep.bbs[8].exprIds.push_back(func->ssaRep.addExpr(&retexpr));
	
	//transformers[8]->doTransformation(binary, func);
	delete func;

	func = new Function();
	for (int i = 0; i < 7; i++) {
		func->ssaRep.bbs.emplace_back();
	}
	PATH(func, 1, 2);
	PATH(func, 2, 3);
	PATH(func, 2, 4);
	PATH(func, 3, 4);
	PATH(func, 4, 5);
	PATH(func, 4, 6);
	PATH(func, 5, 7);
	PATH(func, 6, 7);
	expr.subExpressions = { SSAArgument::createBlock(2) };
	func->ssaRep.bbs[1].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(3), SSAArgument::createUVal(1, 8), SSAArgument::createBlock(4) };
	func->ssaRep.bbs[2].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(4) };
	func->ssaRep.bbs[3].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(5), SSAArgument::createUVal(1, 8), SSAArgument::createBlock(6) };
	func->ssaRep.bbs[4].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(7) };
	func->ssaRep.bbs[5].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(7) };
	func->ssaRep.bbs[6].exprIds.push_back(func->ssaRep.addExpr(&expr));
	func->ssaRep.bbs[7].exprIds.push_back(func->ssaRep.addExpr(&retexpr));
	//transformers[8]->doTransformation(binary, func);
	delete func;

	func = new Function();
	for (int i = 0; i < 4; i++) {
		func->ssaRep.bbs.emplace_back();
	}
	PATH(func, 1, 2);
	PATH(func, 2, 3);
	PATH(func, 2, 4);
	PATH(func, 3, 3);
	PATH(func, 3, 4);
	expr.subExpressions = { SSAArgument::createBlock(2) };
	func->ssaRep.bbs[1].exprIds.push_back(func->ssaRep.addExpr(&expr));
	expr.subExpressions = { SSAArgument::createBlock(3), SSAArgument::createUVal(1, 8), SSAArgument::createBlock(4) };
	func->ssaRep.bbs[2].exprIds.push_back(func->ssaRep.addExpr(&expr));
	func->ssaRep.bbs[3].exprIds.push_back(func->ssaRep.addExpr(&expr));
	func->ssaRep.bbs[4].exprIds.push_back(func->ssaRep.addExpr(&retexpr));
	//transformers[8]->doTransformation(binary, func);
	delete func;

#undef PATH

	delete optimizer;

	return 0;
}
