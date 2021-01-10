#include "WebAssembly.h"

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DebugInfoMetadata.h"

// TODO: Prune unnecessary
#include "MCTargetDesc/WebAssemblyMCTargetDesc.h"
#include "WebAssembly.h"
#include "WebAssemblyExceptionInfo.h"
#include "WebAssemblySortRegion.h"
#include "WebAssemblySubtarget.h"
#include "WebAssemblyUtilities.h"
#include "llvm/ADT/PriorityQueue.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <vector>
#include <algorithm>

static std::string ReplaceString(std::string subject, const std::string& search,
                          const std::string& replace) {
    size_t pos = 0;
    while((pos = subject.find(search, pos)) != std::string::npos) {
         subject.replace(pos, search.length(), replace);
         pos += replace.length();
    }
    return subject;
}

namespace llvm {

struct EmitFunctionGraphDataPass : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  std::string destinationFile;
  EmitFunctionGraphDataPass(const std::string &destinationFile) : FunctionPass(ID), destinationFile(destinationFile)
  {
    RecordFilename(""); // Make sure empty filename is at first index 0.
  }

  bool runOnFunction(Function &Func) override;

  StringRef getPassName() const override { return "EmitFunctionGraphData"; }

  std::map<std::string, int> functionsAdded;
  std::vector<std::string> functions;

  std::map<std::string, int> filenamesAdded;
  std::vector<std::string> filenames;

  struct FunctionCall
  {
    int functionIndex;
    int lineNumber;
    int columnNumber;
  };

  struct FunctionEntry
  {
    int functionIndex;
    int filenameIndex;
    int lineNumber;

    std::vector<FunctionCall> calls;
  };
  std::vector<FunctionEntry> entries;

  int RecordFilename(const std::string &filename)
  {
    auto iter = filenamesAdded.find(filename);
    if (iter != filenamesAdded.end()) return iter->second;
    int idx = (int)filenames.size();
    filenamesAdded[filename] = idx;
    filenames.push_back(filename);
    return idx;
  }

  int RecordFunction(const std::string &function)
  {
    auto iter = functionsAdded.find(function);
    if (iter != functionsAdded.end()) return iter->second;
    int idx = (int)functions.size();
    functionsAdded[function] = idx;
    functions.push_back(function);
    return idx;
  }

  virtual bool doFinalization(Module &) override
  {
    FILE *outFile = fopen(destinationFile.c_str(), "wb");
    if (!outFile)
    {
      errs() << "EmitFunctionGraphData: Failed to open file " << destinationFile << " for writing!\n";
      report_fatal_error("EmitFunctionGraphData: Failed to open output file!");
    }
    fputs("{\n\"functionNames\": [\n", outFile);
    for(size_t i = 0; i < functions.size(); ++i)
    {
      fprintf(outFile, "\"%s\"", functions[i].c_str());
      if (i+1 < functions.size()) fputs(",\n", outFile);
    }
    fputs("\n],\n", outFile);

    fputs("\n\"filenames\": [\n", outFile);
    for(size_t i = 0; i < filenames.size(); ++i)
    {
      fprintf(outFile, "\"%s\"", ReplaceString(filenames[i], "\\", "\\\\").c_str());
      if (i+1 < filenames.size()) fputs(",\n", outFile);
    }
    fputs("\n],\n", outFile);

    fputs("\n\"functions\": [\n", outFile);
    for(size_t i = 0; i < entries.size(); ++i)
    {
      const FunctionEntry &e = entries[i];
      fprintf(outFile, "{\"n\":%d", e.functionIndex);
      if (e.filenameIndex) fprintf(outFile, ",\"f\":%d", e.filenameIndex);
      if (e.lineNumber) fprintf(outFile, ",\"l\":%d", e.lineNumber);
      if (!e.calls.empty())
      {
        fputs(",\"c\":[",outFile);
        for(size_t j = 0; j < e.calls.size(); ++j)
        {
          const FunctionCall &c = e.calls[j];
          fprintf(outFile, "{\"n\":%d", c.functionIndex);
          if (c.lineNumber) fprintf(outFile, ",\"l\":%d", c.lineNumber);
          if (c.columnNumber) fprintf(outFile, ",\"c\":%d", c.columnNumber);
          fputs("}", outFile);
          if (j+1 < e.calls.size()) fputs(",", outFile);
        }
        fputs("]",outFile);
      }
      fputs("}", outFile);
      if (i+1 < entries.size()) fputs(",\n", outFile);
    }
    fputs("\n]\n}\n", outFile);

    fclose(outFile);
    return false;
  }
};

char EmitFunctionGraphDataPass::ID = 0;

#ifdef _WIN32
#define PATH_DELIMITER '\\'
#else
#define PATH_DELIMITER '/'
#endif

static bool IsAbsolutePath(const std::string &filename)
{
  if (filename.length() == 0) return false;
  if (filename[0] == '/') return true;
  if (filename.length() < 2) return false;
  return filename[1] == ':';
}

std::vector<std::string> StringSplit(std::string str, std::string token)
{
  std::vector<std::string> result;
  while(str.size())
  {
    size_t index = str.find(token);
    if (index != std::string::npos)
    {
      result.push_back(str.substr(0,index));
      str = str.substr(index + token.size());
      if (str.size() == 0) result.push_back(str);
    }
    else
    {
      result.push_back(str);
      str = "";
    }
  }
  return result;
}

std::string NormalizePath(std::string filename)
{
  if (filename.length() == 0) return std::string();
#ifdef _WIN32
  filename = ReplaceString(filename, "/", "\\");
#endif
  std::vector<std::string> dirs = StringSplit(filename, std::string()+PATH_DELIMITER);
  size_t i = 0;
  while(i < dirs.size())
  {
    if (dirs[i] == "." || dirs[i].length() == 0)
    {
      dirs.erase(dirs.begin()+i);
    }
    else if (dirs[i] == ".." && i > 0 && dirs[i-1] != "..")
    {
      if (i-1 == 0 && dirs[i-1].back() == ':') // C:\..\foo -> C:\foo
      {
        dirs.erase(dirs.begin()+i);
      }
      else
      {
        dirs.erase(dirs.begin()+i-1, dirs.begin()+i+1);
        --i;
      }
    }
    else
      ++i;
  }
  if (dirs.size() == 1) return dirs.front();
  std::string joined = "";
  for(size_t i = 0; i + 1 < dirs.size(); ++i)
  {
    joined += dirs[i];
    joined += PATH_DELIMITER;
  }
  joined += dirs.back();
  return joined;
}

std::string GetFunctionFileLine(Function &Func, int &outLineNumber)
{
  DISubprogram *subProgram = Func.getSubprogram();
  if (subProgram)
  {
    outLineNumber = subProgram->getLine();
    std::string filename = NormalizePath(subProgram->getFilename().str());
    if (IsAbsolutePath(filename))
      return filename;
    else
    {
      std::string directory = NormalizePath(subProgram->getDirectory().str());
      if (directory.length() > 0 && directory.back() != PATH_DELIMITER)
        return NormalizePath(directory + PATH_DELIMITER + filename);
      else
        return NormalizePath(directory + filename);
    }
  }
  outLineNumber = 0;
  return "";
}

bool EmitFunctionGraphDataPass::runOnFunction(Function &Func) {
  FunctionEntry fe;
  fe.functionIndex = RecordFunction(Func.getName().str());
  fe.filenameIndex = RecordFilename(GetFunctionFileLine(Func, fe.lineNumber));

  for (BasicBlock &BB : Func)
  {
    for (auto BI = BB.begin(), BE = BB.end(); BI != BE;)
    {
      Instruction *I = &*BI++;
      Function *target = 0;
      if (auto *Call = dyn_cast<CallInst>(I)) target = Call->getCalledFunction();
      else if (auto *Call = dyn_cast<InvokeInst>(I)) target = Call->getCalledFunction();
      if (target)
      {
        FunctionCall call = {};
        auto &Loc = I->getDebugLoc();
        if (Loc)
        {
          call.lineNumber = Loc.getLine();
          call.columnNumber = Loc.getCol();
        }
        call.functionIndex = RecordFunction(target->getName().str());
        fe.calls.push_back(call);
      }
    }
  }

  entries.push_back(fe);
  return false;
}

extern FunctionPass *createEmitFunctionGraphDataPass(std::string outputJsonFile) {
  return new EmitFunctionGraphDataPass(outputJsonFile);
}


} // End llvm namespace
