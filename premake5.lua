outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

workspace "DingoNet"
	configurations { "Debug", "Release", "Distribution" }
	startproject "DingoNet-Tests"
	language "C++"
	cppdialect "C++20"
	staticruntime "Off"
	architecture "x86_64"
	multiprocessorcompile "On"
	debugformat "c7"
	warnings "Extra"
	fatalwarnings { "All" }

	filter "system:windows"
		toolset "v145"
		systemversion "latest"
		defines { "DN_PLATFORM_WINDOWS" }
		buildoptions { "/utf-8" }

	filter "system:linux"
		toolset "gcc"
		defines { "DN_PLATFORM_LINUX" }

	filter "configurations:Debug"
		runtime "Debug"
		symbols "On"
		defines { "DN_DEBUG" }

	filter "configurations:Release"
		runtime "Release"
		optimize "On"
		symbols "On"
		defines { "DN_RELEASE" }

	filter "configurations:Distribution"
		runtime "Release"
		optimize "On"
		symbols "Off"
		defines { "DN_DIST" }

	filter {}

project "DingoNet"
	kind "StaticLib"
	targetdir ("%{wks.location}/build/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/build/bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"include/DingoNet/**.h",
		"src/**.h",
		"src/**.cpp"
	}

	includedirs
	{
		"include",
		"src"
	}

project "DingoNet-Tests"
	kind "ConsoleApp"
	location "tests"
	targetdir ("%{wks.location}/build/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/build/bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"tests/**.h",
		"tests/**.cpp"
	}

	includedirs
	{
		"include",
		"src"
	}

	links { "DingoNet" }
