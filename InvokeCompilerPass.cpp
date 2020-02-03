#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <windows.h>

namespace std_filesystem = std::experimental::filesystem::v1;

constexpr bool kIsWin64 =
	#ifdef _WIN64
		true;
	#else
		false;
	#endif
	;

#define error_wprintf(...) fwprintf_s(stderr, L"ERROR: " __VA_ARGS__)

using InvokeCompilerPassFunc  = BOOL (WINAPI*)(int argc, const char** argv, bool);
using InvokeCompilerPassWFunc = BOOL (WINAPI*)(int argc, const wchar_t** argv, bool, HMODULE* phCLUIMod);

static constexpr const char* GetInvokeCompilerPassFuncExportName()
{
	return kIsWin64
		? "InvokeCompilerPass"
		: "_InvokeCompilerPass@12";
}
static constexpr const char* GetInvokeCompilerPassWFuncExportName()
{
	return kIsWin64
		? "InvokeCompilerPassW"
		: "_InvokeCompilerPassW@16";
}

// https://stackoverflow.com/questions/30504/programmatically-retrieve-visual-studio-install-directory
class c_visual_studio_common_tools_path_finder
{
	//VS110COMNTOOLS=C:\Program Files (x86)\Microsoft Visual Studio 11.0\Common7\Tools\

	enum
	{
		k_visual_studio_version_search_start = 11,
		k_visual_studio_version_search_end = 15,
	};
	// cached result for when we fail to find ANY common tools paths
	bool m_found_no_paths;
	std::map<int, std::wstring> m_found_visual_studio_common_tools_paths;
	std::map<int, std::wstring> m_found_visual_cpp_bin_paths;

	static constexpr int ShortVsVersionToFull(int version) { return version * 10; }

	void FindByGetEnvUsingShotguns()
	{
		wchar_t envValueBuffer[_MAX_PATH];

		for (int version = k_visual_studio_version_search_start; version <= k_visual_studio_version_search_end; version++)
		{
			int fullVersion = ShortVsVersionToFull(version);

			//VS110COMNTOOLS
			std::wstring envName(L"VS");
			{
				envName.append(std::to_wstring(fullVersion));
				envName.append(L"COMNTOOLS");
			}

			memset(envValueBuffer, 0, sizeof(envValueBuffer));

			size_t envValueBufferRequiredSize;
			if (_wgetenv_s(&envValueBufferRequiredSize, envValueBuffer, envName.c_str()) != 0 ||
				envValueBufferRequiredSize == 0)
			{
				continue;
			}

			m_found_visual_studio_common_tools_paths.try_emplace(fullVersion, std::wstring(envValueBuffer));
		}

		m_found_no_paths = m_found_visual_studio_common_tools_paths.empty();
	}

public:
	c_visual_studio_common_tools_path_finder()
		: m_found_no_paths(false)
		, m_found_visual_studio_common_tools_paths()
		, m_found_visual_cpp_bin_paths()
	{
	}

	const std::map<int, std::wstring>& GetFoundVisualCppBinPaths() const
	{
		return m_found_visual_cpp_bin_paths;
	}

	bool GetLatestVisualCppBinPath(std_filesystem::path& outPath) const
	{
		if (!GetFoundVisualCppBinPaths().empty())
		{
			outPath = GetFoundVisualCppBinPaths().crbegin()->second;
			return true;
		}

		return false;
	}

	bool FindAllPaths()
	{
		if (m_found_visual_studio_common_tools_paths.empty() && m_found_no_paths==false)
		{
			FindByGetEnvUsingShotguns();
		}
		return !m_found_visual_studio_common_tools_paths.empty();
	}

	bool FilterFoundPathsToAllWithVisualCppInstalled()
	{
		if (!FindAllPaths())
		{
			return false;
		}

		for (auto& pair : m_found_visual_studio_common_tools_paths)
		{
			std_filesystem::path visualStudioCommonToolsPath(pair.second);

			assert(visualStudioCommonToolsPath.filename()==L"."); // this will fail if there's no final slash
			std_filesystem::path visualCppBinPath = visualStudioCommonToolsPath
				.parent_path() // escape the final slash
				.parent_path() // escape Tools
				.parent_path() // escape Common7
				;

			visualCppBinPath.append(L"VC");
			visualCppBinPath.append(L"bin");
			if (kIsWin64)
			{
				visualCppBinPath.append(L"amd64");
			}

			if (!std_filesystem::exists(visualCppBinPath))
			{
				continue;
			}

			m_found_visual_cpp_bin_paths.try_emplace(pair.first, visualCppBinPath);
		}

		return m_found_visual_cpp_bin_paths.empty()==false;
	}
};

class c_visual_cpp_bin_interface
{
	const c_visual_studio_common_tools_path_finder& m_fully_formed_vs_finder;
	std_filesystem::path m_visual_cpp_bin_path;
	std_filesystem::path m_c1_dll_path;
	std_filesystem::path m_cl_ui_dll_path;

public:
	c_visual_cpp_bin_interface(
		const c_visual_studio_common_tools_path_finder& fullyFormedVsFinder)
		: m_fully_formed_vs_finder(fullyFormedVsFinder)
	{
	}

	const std_filesystem::path& GetBinPath() const { return m_visual_cpp_bin_path; }
	const std_filesystem::path& GetDllPathForC1() const { return m_c1_dll_path; }
	const std_filesystem::path& GetDllPathForCLUI() const { return m_cl_ui_dll_path; }

	bool Setup()
	{
		if (!m_fully_formed_vs_finder.GetLatestVisualCppBinPath(m_visual_cpp_bin_path))
			return false;

		m_c1_dll_path = m_visual_cpp_bin_path;
		m_c1_dll_path.append(L"c1.dll");

		m_cl_ui_dll_path = m_visual_cpp_bin_path;
		m_cl_ui_dll_path.append(L"1033");
		m_cl_ui_dll_path.append(L"clui.dll");

		if (!std_filesystem::exists(m_c1_dll_path))
			return false;

		if (!std_filesystem::exists(m_cl_ui_dll_path))
			return false;

		return true;
	}
};

class c_visual_cpp_c1_interface
{
	const c_visual_cpp_bin_interface& m_bin_interface;
	HMODULE m_c1_module;
	HMODULE m_cl_ui_module;
	InvokeCompilerPassWFunc m_c1_invoke_compiler_pass_wide_func;

public:
	c_visual_cpp_c1_interface(
		const c_visual_cpp_bin_interface& binInterface)
		: m_bin_interface(binInterface)
		, m_c1_module(nullptr)
		, m_cl_ui_module(nullptr)
		, m_c1_invoke_compiler_pass_wide_func(nullptr)
	{
	}
	~c_visual_cpp_c1_interface()
	{
		if (m_c1_module != nullptr)
		{
			FreeLibrary(m_c1_module);
			m_c1_module = nullptr;
		}
		if (m_cl_ui_module != nullptr)
		{
			FreeLibrary(m_cl_ui_module);
			m_cl_ui_module = nullptr;
		}

		m_c1_invoke_compiler_pass_wide_func = nullptr;
	}

	HMODULE GetCLUIModule() const { return m_cl_ui_module; }
	InvokeCompilerPassWFunc GetInvokeCompilerPassWideFunc() const { return m_c1_invoke_compiler_pass_wide_func; }

	bool Setup()
	{
		const std_filesystem::path& visualCppBinPathDir = m_bin_interface.GetBinPath();

		if (!SetDllDirectoryW(visualCppBinPathDir.generic_wstring().c_str()))
			return false;

		return true;
	}

	bool LoadC1()
	{
		std::wstring c1DllPathString = m_bin_interface.GetDllPathForC1().generic_wstring();
		m_c1_module = LoadLibraryW(c1DllPathString.c_str());
		if (m_c1_module == nullptr)
			return false;

		m_c1_invoke_compiler_pass_wide_func = (InvokeCompilerPassWFunc)GetProcAddress(m_c1_module, GetInvokeCompilerPassWFuncExportName());
		if (m_c1_invoke_compiler_pass_wide_func == nullptr)
			return false;

		return true;
	}
	bool LoadCLUI()
	{
		std::wstring cluiPathString = m_bin_interface.GetDllPathForCLUI().generic_wstring();
		m_cl_ui_module = LoadLibraryExW(cluiPathString.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);

		return m_cl_ui_module != nullptr;
	}
};

class c_visual_cpp_c1_preprocessor_interface
{
	const c_visual_cpp_c1_interface& m_c1_interface;
	std::vector<const wchar_t*> m_invoke_func_args;
	std::vector<std::wstring> m_defines;
	std::vector<std::wstring> m_include_paths;
	std::vector<std::wstring> m_file_paths;
	int m_errors_returned;

	bool m_preserve_comments;

	void FinalizeArgs()
	{
		m_invoke_func_args.push_back(L"-nologo");
		m_invoke_func_args.push_back(L"-E");	// preprocess
		m_invoke_func_args.push_back(L"-EP");	// Preprocess to stdout Without #line Directives

		if (m_preserve_comments)
		{
			m_invoke_func_args.push_back(L"-C"); // Preserve Comments During Preprocessing
		}

		for (const std::wstring& str : m_defines)
		{
			m_invoke_func_args.push_back(L"-D");
			m_invoke_func_args.push_back(str.c_str());
		}
		for (const std::wstring& str : m_include_paths)
		{
			// #TODO
			//m_invoke_func_args.push_back(L"-D");
			//m_invoke_func_args.push_back(str.c_str());
		}
		for (const std::wstring& str : m_file_paths)
		{
			m_invoke_func_args.push_back(L"-f");
			m_invoke_func_args.push_back(str.c_str());
		}
	}

public:
	c_visual_cpp_c1_preprocessor_interface(
		const c_visual_cpp_c1_interface& c1Interface)
		: m_c1_interface(c1Interface)
		, m_invoke_func_args()
		, m_defines()
		, m_file_paths()
		, m_errors_returned(-1)
		, m_preserve_comments(false)
	{
	}

	// Whether or no C based comments are preserved
	void SetPreserveComments(bool value) { m_preserve_comments = value; }

	bool Run()
	{
		assert(m_errors_returned == -1); // You did not Reset me before calling, shame!

		FinalizeArgs();

		auto hCLUIModule = m_c1_interface.GetCLUIModule();
		auto invoke_func = m_c1_interface.GetInvokeCompilerPassWideFunc();
		m_errors_returned = invoke_func(static_cast<int>(m_invoke_func_args.size()), m_invoke_func_args.data(), false, &hCLUIModule);
		if (hCLUIModule != m_c1_interface.GetCLUIModule())
		{
			assert(!"CL pulled the UI module rug out from underneath me :(");
		}

		return m_errors_returned == 0;
	}

	void Reset()
	{
		m_invoke_func_args.clear();
		m_defines.clear();
		m_include_paths.clear();
		m_file_paths.clear();
		m_errors_returned = -1;
	}

	void AddDefine(const wchar_t* define)
	{
		m_defines.emplace_back(define);
	}
	void AddIncludePath(const wchar_t* includePath)
	{
		m_include_paths.emplace_back(includePath);
	}
	void AddFilePath(const wchar_t* filePath)
	{
		m_file_paths.emplace_back(filePath);
	}
};

// http://blog.airesoft.co.uk/2013/01/plug-in-to-cls-kitchen/
// https://docs.microsoft.com/en-us/windows/desktop/procthread/creating-a-child-process-with-redirected-input-and-output maybe?
int main()
{
	int exit_code = -1;

	FILE* redirectedOutput = nullptr;

	do
	{
		c_visual_studio_common_tools_path_finder visualStudioPathsFinder;
		if (!visualStudioPathsFinder.FilterFoundPathsToAllWithVisualCppInstalled())
		{
			error_wprintf(L"Failed to find any VC++ installations\n");
			error_wprintf(L"GetLastError(0x%08X)\n", GetLastError());
			break;
		}

		c_visual_cpp_bin_interface visualCppBinInterface(visualStudioPathsFinder);
		if (!visualCppBinInterface.Setup())
		{
			error_wprintf(L"Failed to find all VC++ bin components\n");
			error_wprintf(L"GetLastError(0x%08X)\n", GetLastError());
			break;
		}

		c_visual_cpp_c1_interface visualCppC1Interface(visualCppBinInterface);
		if (!visualCppC1Interface.Setup())
		{
			error_wprintf(L"Failed to setup environment for c1 interface\n");
			error_wprintf(L"GetLastError(0x%08X)\n", GetLastError());
			break;
		}

		if (!visualCppC1Interface.LoadC1())
		{
			error_wprintf(L"Failed to load c1.dll\n");
			error_wprintf(L"GetLastError(0x%08X)\n", GetLastError());
			break;
		}

		if (!visualCppC1Interface.LoadCLUI())
		{
			error_wprintf(L"Failed to load clui.dll\n");
			error_wprintf(L"GetLastError(0x%08X)\n", GetLastError());
			break;
		}

		c_visual_cpp_c1_preprocessor_interface invokePreprocessor(visualCppC1Interface);
		if (false)
		{
			invokePreprocessor.AddDefine(L"NDEBUG");
		}
		if (false)
		{
			invokePreprocessor.SetPreserveComments(true);
		}
		if (true)
		{
			invokePreprocessor.AddFilePath(LR"(test_input.hspp)");
		}

		// redirect stdout to a file, for preprocessor results
		// NOTE: the preprocessor code ends up writing a newline to stdout before writing any of the file's lines :(
		if (true)
		{
			fflush(stdout); // just to be safe I suppose
			errno_t open_error = _wfreopen_s(&redirectedOutput, LR"(test_output.hsc)", L"wt", stdout);
			if (redirectedOutput == nullptr)
			{
				error_wprintf(L"Failed _wfreopen_s\n");
				error_wprintf(L"errno_t(0x%08X)\n", open_error);
				break;
			}
		}

		if (!invokePreprocessor.Run())
		{
			error_wprintf(L"Failed while running c1 preprocessor\n");
			error_wprintf(L"GetLastError(0x%08X)\n", GetLastError());
			exit_code = 1;
			break;
		}

		exit_code = 0;
	} while (false);

	if (redirectedOutput != nullptr)
	{
		fflush(redirectedOutput);
		fclose(redirectedOutput);
		// https://stackoverflow.com/a/22574517/444977
		FILE* reopenedStdOut;
		_wfreopen_s(&reopenedStdOut, L"CONOUT$", L"wt", stdout);
		wprintf_s(L"Done!");
	}

	return exit_code;
}