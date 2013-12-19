/* 
 * San Andreas modloader
 * Copyright (C) 2013  LINK/2012 <dma_2012@hotmail.com>
 * Licensed under GNU GPL v3, see LICENSE at top level directory.
 * 
 *  Modloader core, main source file
 *  The core do not handle any file at all, file handling is plugin-based
 * 
 */

#include <modloader.hpp>
#include <modloader_util_path.hpp>
#include <modloader_util_container.hpp>

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "Injector.h"
#include "GameInfo.h"
#include "CModLoader.hpp"

#if 0 && !defined(NDEBUG)
#define LOGFILE_AS_STDOUT
#endif


/*
 * TODO: Plugin Priority Config File
 *       ~~~~~~ Exclusion Config File
 *      Export ReadModf & LoadPlugin / UnloadPlugin ?
 *      Export FindFileHandler ?
 *      mods sub-recursion. E.g. when a mod contains a modloader folder, load mods from it's folder
 * 
 *
 */

namespace modloader
{
    static const char* modurl = "https://github.com/thelink2012/sa-modloader";
    
    // log stream
    static FILE* logfile = 0;
    
    /*
     * Log
     *      Logs something into the log file 'logfile'
     *      @msg: (Format) Message to log
     *      @...: Additional args to format
     */
    void Log(const char* msg, ...)
    {
        if(logfile)
        {
            va_list va; va_start(va, msg);
            vfprintf(logfile, msg, va);
            fputc('\n', logfile);
            fflush(logfile);
            va_end(va);
        }
    }
    
    /*
     * Error
     *      Displays a error message box
     *      @msg: (Format) Error to display
     *      @...: Additional args to format
     */
    void Error(const char* msg, ...)
    {
        va_list va; va_start(va, msg);
        char buffer[1024];
        vsprintf(buffer, msg, va);
        MessageBoxA(NULL, buffer, "modloader", MB_ICONERROR); 
        va_end(va);
    }

}

using namespace modloader;

// Singleton
static CModLoader loader;





/*
 * DllMain
 *      Second entry-point, yeah, really
 */
extern "C" __declspec(dllexport) // Needed on MinGW...
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    // Startup or Shutdown modloader
    bool bResult = true; char buffer[256];
    
    switch(fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            loader.Patch();
            break;
            
        case DLL_PROCESS_DETACH:
            bResult = loader.Shutdown();    /* Shutdown the loader if it hasn't shutdown yet */
            break;
    }
    return bResult;
}


namespace modloader
{
    static LPTOP_LEVEL_EXCEPTION_FILTER PrevFilter = 0;
    
    static LONG CALLBACK modloader_UnhandledExceptionFilter(LPEXCEPTION_POINTERS pException)
    {
        LogException(pException);
        
        // We should shutdown our loader at all cost
        Log("Calling loader.Shutdown();");
        loader.Shutdown();
        
        // Continue exception propagation
        return (PrevFilter? PrevFilter(pException) : EXCEPTION_CONTINUE_SEARCH);  // I'm not really sure about this return
    }
    
    typedef int (CALLBACK *WinMain_t)(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow); 
    static WinMain_t WinMainPtr;        // address original WinMain function is at
    static void* WinMainIsCalledAt;     // address the instruction "call WinMain" is at
    
    /*
     *  Our hook at game's WinMain call, modloader startup happens here
     */
    static int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
    {
        /* Setup exception filter, we need (whenever possible) to call shutdown before a crash or something */
        PrevFilter = SetUnhandledExceptionFilter(modloader_UnhandledExceptionFilter);
        
        /* Startup the loader */
        loader.Startup();

        /*
         *  Call the original call
         *  Use the original address if after loader.Startup() the instruction "call WinMain" hasn't changed it's call address.
         *  An ASI may have changed it
         */
        WinMain_t call = (WinMain_t) ReadRelativeOffset((char*)WinMainIsCalledAt + 1).p;
        call = (call != WinMain? call : WinMainPtr);
        int result = call(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
        
        /* Shutdown the loader */
        loader.Shutdown();
        
        return result;
    }
    
    
    
     
    
    /*
     * CModLoader::Startup
     *      Start ups modloader, loading plugins and other stuffs
     */
    bool CModLoader::Startup()
    {
        // If already started up or modloader directory does not exist, do nothing
        if(this->bWorking == false && IsDirectoryA("modloader"))
        {
            char gameFolder[MAX_PATH * 2];
            
            /* Open log file */
#ifndef LOGFILE_AS_STDOUT
            if(!logfile) logfile = fopen("modloader/modloader.log", "w");
#else
            logfile = stdout;
#endif
            
            /* Log header, with version number and isdev information */
            Log("========================== modloader %d.%d.%d %s==========================\n",
                MODLOADER_VERSION_MAJOR, MODLOADER_VERSION_MINOR, MODLOADER_VERSION_REVISION,
                MODLOADER_VERSION_ISDEV? "Development Build " : "");

            /* --- */
            GetCurrentDirectoryA(sizeof(gameFolder), gameFolder);
            this->gamePath = gameFolder;
            MakeSureStringIsDirectory(this->gamePath);

            /* Make sure the main paths do exist */
            {
                static const char* folders[] =
                {
                    "modloader/.data",
                    "modloader/.data/plugins",
                    "modloader/.data/cache",
                    0
                };
                
                this->modsPath = "modloader\\";
                this->cachePath = this->modsPath + ".data\\cache\\";
                
                for(const char** folder = folders; *folder; ++folder)
                {
                    Log("Making sure directory \"%s\" exists...", *folder);
                    if(!MakeSureDirectoryExistA(*folder)) Log("\t...it didn't, created it.");
                }
            }
            
            /* Register modloader methods and vars */
            this->loader.gamepath  = this->gamePath.c_str();
            this->loader.modspath  = this->modsPath.c_str();
            this->loader.cachepath = this->cachePath.c_str();
            this->loader.Log   = &Log;
            this->loader.Error = &Error;
            
            
            // Do init
            this->LoadPlugins();        /* Load plugins, such as std-img.dll at /modloader/.data/plugins */
            this->StartupPlugins();     /* Call Startup methods from plugins */
            this->PerformSearch();      /* Search for mods at /modloader, but do not install them yet */
            this->HandleFiles();        /* Install mods found on search above */
            this->PosProcess();         /* After mods are installed, notify all plugins, they may want to do some pos processing */
            this->ClearFilesData();     /* Clear files data (such as paths) freeing memory */
            
            Log("\nGame is ready!\n");

            /* Just make sure we're in the main folder before going back */
            SetCurrentDirectoryA(gameFolder);

            this->bWorking = true;
            return true;
        }
        else { return false; }
    }

    /*
     * CModLoader::OnShutdown
     *  Shutdowns modloader, unloading plugins and other stuffs
     */
    bool CModLoader::Shutdown()
    {
        /* Don't shutdown if not started up or already shut down */
        if(this->bWorking == false)
            return true;
        
        /* Set dir to be the make dir */
        
        SYSTEMTIME time;
        GetLocalTime(&time);
        
        Log("\nShutdowing modloader...");
        this->UnloadPlugins();
        Log("modloader has been shutdown.");
        
		Log(
			"\n*********************************\n"
			"> Logging finished: %.2d:%.2d:%.2d\n"
			"  Powered by sa-modloader (%s)\n"
			"*********************************\n",
			time.wHour, time.wMinute, time.wSecond,
            modurl);

        
#ifndef LOGFILE_AS_STDOUT       
        if(logfile) { fclose(logfile); logfile = 0; }
#endif
        
        this->bWorking = false;
        return true;
    }

    /*
     * CModLoader::Patch
     *      Patches the game code
     */
    void CModLoader::Patch()
    {
        static GameInfo gameInfo;
        gameInfo.PluginName = "modloader";
        
        // I can't use LoadLibrary at DllMain, so use it somewhere at the beggining, before the startup of the game engine
        gameInfo.DelayedDetect([](GameInfo& info)
        {
            // Shit, we still don't support a delayed style of detection, how to handle it? Help me!
            if(info.IsDelayed())
                Error("Modloader does not support this executable version [DELAYED_DETECT_ERROR]");
            //--
            else if(info.GetGame() != info.SA)
                Error("Modloader was built for GTA San Andreas! This game is not supported.");
            else if(info.GetMajorVersion() != 1 && info.GetMinorVersion() != 0)
                Error("Modloader still do not support other versioons than HOODLUM GTA SA 1.0");
            else
            {
                WinMainPtr = (WinMain_t) MakeCALL(WinMainIsCalledAt = (void*)(0x8246EC), (void*) WinMain).p;
            }
            
        }, true);
    }

    /*
     * CModLoader::PosProcess
     *      Call all 'plugin.PosProcess' methods
     */   
    void CModLoader::PosProcess()
    {
        CSetCurrentDirectory xdir(this->gamePath.c_str());
        Log("\nPos processing...");
        for(auto& plugin : this->plugins)
        {
            Log("Pos processing plugin \"%s\"", plugin.name);
            if(plugin.PosProcess(&plugin))
                Log("Plugin \"%s\" failed to pos process", plugin.name);
        }
    }
    
    /*
     * CModLoader::LoadPlugins
     *      Loads all plugins
     */
    void CModLoader::LoadPlugins()
    {
        Log("\nLooking for plugins...");

        /* Goto plugins folder */
        if(SetCurrentDirectoryA("modloader\\.data\\plugins\\"))
        {       
            ForeachFile("*.dll", false, [this](ModLoaderFile& file)
            {
                LoadPlugin(file.filepath, false);
                return true;
            });

            this->SortPlugins();
            this->BuildExtensionMap();
            
            /* Go back to main folder */
            SetCurrentDirectory("..\\..\\");
        }
    }

    /*
     * CModLoader::UnloadPlugins
     *      Unloads all plugins
     */
    void CModLoader::UnloadPlugins()
    {
        // Unload one by one, calling OnShutdown callback before the unload
        for(auto it = this->plugins.begin(); it != this->plugins.end(); ++it)
        {
            this->UnloadPlugin(*it, false);
            it = this->plugins.erase(it);
        }
        // Clear plugin list after all
        this->plugins.clear();
    }
    
    /*
     * CModLoader::LoadPlugin
     *      Load a new plugin
     *      @pluginPath: Plugin path
     */
    bool CModLoader::LoadPlugin(const char* pluginPath, bool bDoStuffNow)
    {
        const char* modulename = pluginPath;
        modloader_fGetPluginData GetPluginData;
        modloader_plugin_t data;
        bool bFail = false;
        HMODULE module;

        // Load plugin module
        if(module = LoadLibraryA(modulename))
        {
            Log("Loading plugin module '%s'", modulename);
            GetPluginData = (modloader_fGetPluginData)(GetProcAddress(module, "GetPluginData"));

            // Setup new's plugin data and fill plugin data from the plugin module
            memset(&data, 0, sizeof(data));
            data.modloader = &this->loader;
            data.pModule   = module;
            data.priority  = 50;
            
            // Check if plugin has been loaded sucessfully
            if(GetPluginData)
            {
                GetPluginData(&data);

                // Check version incompatibilities (ignore for now, no incompatibilities)
                if(false && (data.major && data.minor && data.revision))
                {
                    bFail = true;
                    Log("Failed to load module '%s', version incompatibility detected.", modulename);
                }
                // Check if plugin was written to a (future) version of modloader, if so, we need to be updated
                else if(data.major  > MODLOADER_VERSION_MAJOR
                     ||(data.major == MODLOADER_VERSION_MAJOR && data.minor > MODLOADER_VERSION_MINOR))
                     // We don't check VERSION_REVISION because on revisions we don't intent to break structures, etc
                {
                    bFail = true;
                    Log("Failed to load module '%s', it requieres a newer version of modloader!\n"
                "Update yourself at: %s",
                modulename, modurl);
                }
                else if(data.priority == 0)
                {
                    bFail = true;
                    Log("Plugin module '%s' will not be loaded. It's priority is 0", modulename);
                }
                else
                {
                    data.name = data.GetName? data.GetName(&data) : "NONAME";
                    data.version = data.GetVersion? data.GetVersion(&data) : "NOVERSION";
                    data.author = data.GetAuthor? data.GetAuthor(&data) : "NOAUTHOR";
                    Log("Plugin module '%s' loaded as %s %s by %s", modulename, data.name, data.version, data.author);
                }
            }
            else
            {
                bFail = true;
                Log("Could not call GetPluginData() for module '%s'", modulename);
            }
                
            // On failure, unload module, on success, push plugin to list
            if(bFail)
                FreeLibrary(module);
            else
            {
                this->plugins.push_back(data);
                if(bDoStuffNow)
                {
                    this->StartupPlugin(this->plugins.back());
                    this->SortPlugins();
                }
            }
        }
        else
            Log("Could not load plugin module '%s'", modulename);

        return !bFail;
    }
    

    
    /*
     * CModLoader::UnloadPlugin
     *      Unloads a specific plugin -- (remove plugin from plugin list)
     *      @pluginName: The plugin name
     */
    bool CModLoader::UnloadPlugin(const char* pluginName)
    {
        for(auto it = this->plugins.begin(); it != this->plugins.end(); ++it)
        {
            if(!strcmp(it->name, pluginName, true))
                return this->UnloadPlugin(*it, true);
        }
        
        Log("Could not unload plugin named '%s' because it was not found", pluginName);
        return false;
    }
    
    /*
     *  CModLoader::UnloadPlugin
     *      Unloads a specific plugin
     *      @plugin: The plugin instance
     *      @bRemoveFromList: Removes plugin from this->plugins list,
     *                        when true be warned that iterators to this element will get invalidated!
     */
    bool CModLoader::UnloadPlugin(ModLoaderPlugin& plugin, bool bRemoveFromList)
    {
        CSetCurrentDirectory xdir(this->loader.gamepath);
        
        Log("Unloading plugin \"%s\"", plugin.name);
        
        if(plugin.OnShutdown) plugin.OnShutdown(&plugin);
        if(plugin.pModule) FreeLibrary((HMODULE)(plugin.pModule));
        if(bRemoveFromList) this->plugins.remove(plugin);
        
        return true;
    }
    

    /*
     * CModLoader::PeformSearch
     *      Search for mods
     */
    void CModLoader::PerformSearch()
    {
        /* Iterate on all folders at /modloader/ dir, and treat them as a mod entity */
        Log("\nLooking for mods...");
        CSetCurrentDirectory xdir("modloader\\");
        {
            ForeachFile("*.*", false, [this](ModLoaderFile& file)
            {
                /* Must be a directory to be a mod */
                if(file.is_dir) this->ReadModf(file.filepath);
                return true;
            });
        }
    }

    
    /*
     * CModLoader::ReadModf
     *      Read a mod
     *      @modfolder: Mod folder
     */
    void CModLoader::ReadModf(const char* modfolder_cc)
    {
        std::string modfolder_str = modfolder_cc;
        //if(modfolder_str.compare(, 2, ".\\");
        const char* modfolder = modfolder_str.c_str();
        
        
        /* Go into the mod folder to work inside it */
        {
            CSetCurrentDirectory xdir(modfolder);
            
            char buffer[MAX_PATH * 2];
            GetCurrentDirectoryA(sizeof(buffer), buffer);
 
            /* Push a new modification into the mods list */
            auto& mod = AddNewItemToContainer(this->mods);
            mod.name = &modfolder[GetLastPathComponent(modfolder)];
            mod.id = this->currentModId++;
            mod.path = std::string("modloader\\") + modfolder;
            mod.fullPath = buffer;
            
            Log("Reading mod \"%s\" (%d) at \"%s\"...", mod.name.c_str(), mod.id, modfolder);
            
            ForeachFile("*.*", true, [this, &mod](ModLoaderFile& file)
            {
                this->ReadFile(file, mod);
                return true;
            });
        }     
    }
    
    
    /*
     *  CModLoader::ReadFile
     *      Read file
     */
    void CModLoader::ReadFile(ModLoaderFile& file, CModLoader::ModInfo& mod)
    {
        /* Continue the 'file' setup */
        file.modname = mod.name.data();
        file.mod_id = mod.id;
        file.file_id = this->currentFileId++;
        file.modpath = mod.path.data();
        file.modfullpath = mod.fullPath.data();      
        
        /* See if we can find a handler for this file */
        ModLoaderPlugin* handler = this->FindFileHandler(file);
        
        if(handler)
        {
            /* If this is a directory and a handler for it has been found,
            * don't go inside this folder (recursive search). */
            file.recursion = false;
            
            /* Setup fileInfo */
            auto& fileInfo = AddNewItemToContainer(mod.files);
            fileInfo.id = file.file_id;
            fileInfo.parentMod = &mod;
            fileInfo.handler = handler;
            fileInfo.isDir = file.is_dir;

            /* Continue setupping fileInfo and resetup 'file' to contain pointers to fileInfo (static) */
            file.filename = (fileInfo.fileName = file.filename).data();
            file.filepath = (fileInfo.filePath = file.filepath).data();
            file.filext   = (fileInfo.fileExtension = file.filext).data();
            
            /* Copy C POD data into fileInfo */
            memcpy(&fileInfo.data, &file, sizeof(file));
        }
            
        if(handler)
            Log("Handler for file \"%s\" (%d) is \"%s\"", file.filepath, file.file_id, handler->name);
        else
            Log("Handler for file \"%s\" (%d) not found", file.filepath, file.file_id);    
        

        
    }
    
    /*
     *  CModLoader::ReadFile
     *      Read file
     */
    bool CModLoader::HandleFile(CModLoader::FileInfo& file)
    {
        // Process file using the handler
        if(auto* handler = file.handler)
        {
            const char* pluginName = handler->name;
            const char* filePath   = file.filePath.data(); 
            
            Log("Handling file \"%s\\%s\" by plugin \"%s\"", file.parentMod->name.c_str(), filePath, pluginName);
            if(handler->ProcessFile && !handler->ProcessFile(handler, &file.data))
            {
                return true;
            }
            else
                // That's not my fault, hen
                Log("Handler \"%s\" failed to process file \"%s\"", pluginName, filePath);
        }
        return false;
    }
    
    /*
     * IsFileHandlerForFile
     *      Checks if an plugin can check a specific file. It also sets the @plugin.checked flag.
     *      @plugin: The plugin to handle the file
     *      @file: File to be handled
     */
    inline bool IsFileHandlerForFile(ModLoaderPlugin& plugin, const ModLoaderFile& file)
    {
        return( !plugin.checked         /* Has not been checked yet */
             && (plugin.checked = true) /* Mark as checked */
            
             /* call CheckFile */
             &&  plugin.CheckFile  && (plugin.CheckFile(&plugin, &file) == MODLOADER_YES)
            );
    }
    
    /*
     * CModLoader::FindFileHandler
     *      Finds a plugin to handle the file
     *      @file: File to be handled
     *      @return: Plugin to handle the file, or nullptr if no plugin found
     * 
     *      This uses an algorithim that first searches for plugins that commonly use the @file extension
     *      and if no handler for file has been found, it checks on other plugins.
     */
    ModLoaderPlugin* CModLoader::FindFileHandler(const ModLoaderFile& file)
    {
        ModLoaderPlugin* handler = 0;

        /* First, search for the handler at the vector for the extension,
         * it is commonly possible that the handler for this file is here and we don't have to call CheckFile on all plugins
         * At this point all 'plugin.checked' are false.
         */
        for(auto& pPlugin : this->extMap[file.filext])
        {
            auto& plugin = *pPlugin;    /* Turn into reference just for common coding style */
            if(IsFileHandlerForFile(plugin, file))
            {
                handler = &plugin;
                break;
            }
        }

        /*
         *  Iterate on all plugins to mark all 'plugin.checked' flags as false,
         *  but before, if no handler has been found and a plugin was not checked yet, check it.
         */
        for(auto& plugin : this->plugins)
        {
            if(!handler && IsFileHandlerForFile(plugin, file))
            {
                handler = &plugin;
            }
                
            /* Set the 'plugin.checked' state to false, so on the next call to FindFileHandler it is false */
            plugin.checked = false;
        }
        
        return handler;
    }

}

