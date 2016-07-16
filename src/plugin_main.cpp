/**
 *  (c) 2016 Jonas Zaddach <jonas.zaddach@gmail.com>
 *
 *  This file is part of IDA-LLVM.
 * 
 *  IDA-LLVM is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  IDA-LLVM is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with IDA-LLVM.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <ida.hpp>
#include <idp.hpp>
#include <graph.hpp>
#include <loader.hpp>
#include <kernwin.hpp>

#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Scalar.h>

#include <list>

#include "idallvm/plugin.h"
#include "idallvm/msg.h"
#include "idallvm/string.h"
#include "idallvm/ida_util.h"
#include "idallvm/libqemu.h"
#include "idallvm/plugin_python.h"
#include "idallvm/llvm_passes.h"

using llvm::legacy::FunctionPassManager;
using llvm::succ_iterator;
using llvm::succ_begin;
using llvm::succ_end;
using llvm::createInstructionNamerPass;

extern plugin_t PLUGIN;
//--------------------------------------------------------------------------
static qstrvec_t graph_text;
static ProcessorInformation processor_info;
static FunctionPassManager* functionPassManager = nullptr; 

//--------------------------------------------------------------------------
static const char *get_node_name(int n)
{
  switch ( n )
  {
    case 0: return COLSTR("This", SCOLOR_MACRO);
    case 1: return COLSTR("is", SCOLOR_CNAME);
    case 2: return "a";
    case 3: return COLSTR("sample", SCOLOR_DNAME);
    case 4: return COLSTR("graph", SCOLOR_IMPNAME);
    case 5: return COLSTR("viewer", SCOLOR_ERROR);
    case 6: return COLSTR("window!", SCOLOR_DNUM) "\n(with colorful names)";
  }
  return "?";
}

//--------------------------------------------------------------------------

class IDALLVMFunction
{
private:
    llvm::Function* function;
    std::vector<std::string> nodeText;
    std::map<llvm::BasicBlock*, int> bbToIndex;
    std::list< std::pair<int, int> > edges;
    std::vector<llvm::BasicBlock*> basicBlocks;

public:
    IDALLVMFunction(llvm::Function* function) : function(function) {
        //Run InstructionNamer pass to make printing faster
        functionPassManager->run(*function);
        basicBlocks.reserve(function->size());
        for (llvm::BasicBlock& bb : *function) {
            bbToIndex.insert(std::make_pair(&bb, basicBlocks.size()));
            basicBlocks.push_back(&bb);
        }
    }
    llvm::Function* getFunction(void) {return function;}


    void generateBasicBlockText(void) {
        nodeText.clear();
        nodeText.resize(basicBlocks.size());
        for (int idx = 0; idx < basicBlocks.size(); ++idx) {
            llvm::raw_string_ostream ss(nodeText[idx]);
            //TODO: Make printing more pretty, use COLSTR for colors, nice names, ...
            ss << *basicBlocks[idx];
            ss.flush();
        }
    }
    const char* getBasicBlockText(int idx) {return nodeText.at(idx).c_str();}
    int getBasicBlockIndex(llvm::BasicBlock* bb) {auto itr = bbToIndex.find(bb); return itr != bbToIndex.end() ? itr->second : -1;}
    int getNumBasicBlocks(void) {return basicBlocks.size();}
    std::list< std::pair< int, int > > const& getEdges(void) {
        if (edges.size() == 0) {
            for (int bbIdx = 0; bbIdx < basicBlocks.size(); ++bbIdx) {
                for (succ_iterator sitr = succ_begin(basicBlocks.at(bbIdx)), send = succ_end(basicBlocks.at(bbIdx)); sitr != send; ++sitr) {
                    assert(bbToIndex.find(*sitr) != bbToIndex.end() && "Basic block must be in the map of BBs to indices");
                    edges.push_back(std::make_pair(bbIdx, bbToIndex[*sitr]));
                }
            }
        }
        return edges;
    }
};

static int idaapi callback(void* userdata, int code, va_list va)
{
  IDALLVMFunction* data = static_cast<IDALLVMFunction*>(userdata);
//  msg("callback(userdata = %p, code = %d, va = %p)\n", userdata, code, va);

  int result = 0;
  switch ( code )
  {
    case grcode_calculating_layout:
                              // calculating user-defined graph layout
                              // in: mutable_graph_t *g
                              // out: 0-not implemented
                              //      1-graph layout calculated by the plugin
      msg("calculating graph layout...\n");
      break;

    case grcode_changed_current:
                              // a new graph node became the current node
                              // in:  graph_viewer_t *gv
                              //      int curnode
                              // out: 0-ok, 1-forbid to change the current node
     {
       graph_viewer_t *v = va_arg(va, graph_viewer_t *);
       int curnode       = va_argi(va, int);
       msg("%p: current node becomes %d\n", v, curnode);
     }
     break;

    case grcode_clicked:      // a graph has been clicked
                              // in:  graph_viewer_t *gv
                              //      selection_item_t *current_item
                              // out: 0-ok, 1-ignore click
     {
       graph_viewer_t *v = va_arg(va, graph_viewer_t *); qnotused(v);
       selection_item_t *it = va_arg(va, selection_item_t *); qnotused(it);
       graph_item_t *m = va_arg(va, graph_item_t *);
       msg("clicked on ");
       switch ( m->type )
       {
         case git_none:
           msg("background\n");
           break;
         case git_edge:
           msg("edge (%d, %d)\n", m->e.src, m->e.dst);
           break;
         case git_node:
           msg("node %d\n", m->n);
           break;
         case git_tool:
           msg("toolbutton %d\n", m->b);
           break;
         case git_text:
           msg("text (x,y)=(%d,%d)\n", m->p.x, m->p.y);
           break;
         case git_elp:
           msg("edge layout point (%d, %d) #%d\n", m->elp.e.src, m->elp.e.dst, m->elp.pidx);
           break;
       }
     }
     break;

    case grcode_dblclicked:   // a graph node has been double clicked
                              // in:  graph_viewer_t *gv
                              //      selection_item_t *current_item
                              // out: 0-ok, 1-ignore click
     {
       graph_viewer_t *v   = va_arg(va, graph_viewer_t *);
       selection_item_t *s = va_arg(va, selection_item_t *);
       msg("%p: %sclicked on ", v, code == grcode_clicked ? "" : "dbl");
       if ( s == NULL )
         msg("background\n");
       else if ( s->is_node )
         msg("node %d\n", s->node);
       else
         msg("edge (%d, %d) layout point #%d\n", s->elp.e.src, s->elp.e.dst, s->elp.pidx);
     }
     break;

    case grcode_creating_group:
                              // a group is being created
                              // in:  mutable_graph_t *g
                              //      intvec_t *nodes
                              // out: 0-ok, 1-forbid group creation
     {
       mutable_graph_t *g = va_arg(va, mutable_graph_t *);
       intvec_t &nodes    = *va_arg(va, intvec_t *);
       msg("%p: creating group", g);
       for ( intvec_t::iterator p=nodes.begin(); p != nodes.end(); ++p )
         msg(" %d", *p);
       msg("...\n");
     }
     break;

    case grcode_deleting_group:
                              // a group is being deleted
                              // in:  mutable_graph_t *g
                              //      int old_group
                              // out: 0-ok, 1-forbid group deletion
     {
       mutable_graph_t *g = va_arg(va, mutable_graph_t *);
       int group          = va_argi(va, int);
       msg("%p: deleting group %d\n", g, group);
     }
     break;

    case grcode_group_visibility:
                              // a group is being collapsed/uncollapsed
                              // in:  mutable_graph_t *g
                              //      int group
                              //      bool expand
                              // out: 0-ok, 1-forbid group modification
     {
       mutable_graph_t *g = va_arg(va, mutable_graph_t *);
       int group          = va_argi(va, int);
       bool expand        = va_argi(va, bool);
       msg("%p: %scollapsing group %d\n", g, expand ? "un" : "", group);
     }
     break;

    case grcode_gotfocus:     // a graph viewer got focus
                              // in:  graph_viewer_t *gv
                              // out: must return 0
     {
       graph_viewer_t *g = va_arg(va, graph_viewer_t *);
       msg("%p: got focus\n", g);
     }
     break;

    case grcode_lostfocus:    // a graph viewer lost focus
                              // in:  graph_viewer_t *gv
                              // out: must return 0
     {
       graph_viewer_t *g = va_arg(va, graph_viewer_t *);
       msg("%p: lost focus\n", g);
     }
     break;

    case grcode_user_refresh: // refresh user-defined graph nodes and edges
                              // in:  mutable_graph_t *g
                              // out: success
     {
       mutable_graph_t *g = va_arg(va, mutable_graph_t *);
       msg("%p: refresh\n", g);
       // our graph is like this:
       //  0 -> 1 -> 2
       //       \-> 3 -> 4 -> 5 -> 6
       //           ^        /
       //           \-------/
       if (g->empty()) {
           g->resize(data->getNumBasicBlocks());
       }
       for (std::pair< int, int > const& edge : data->getEdges()) {
           g->add_edge(edge.first, edge.second, NULL);
       }
       result = true;
     }
     break;

    case grcode_user_gentext: // generate text for user-defined graph nodes
                              // in:  mutable_graph_t *g
                              // out: must return 0
     {
       mutable_graph_t *g = va_arg(va, mutable_graph_t *);
       msg("%p: generate text for graph nodes\n", g);
       data->generateBasicBlockText();
       result = true;
       qnotused(g);
     }
     break;

    case grcode_user_text:    // retrieve text for user-defined graph node
                              // in:  mutable_graph_t *g
                              //      int node
                              //      const char **result
                              //      bgcolor_t *bg_color (maybe NULL)
                              // out: must return 0, result must be filled
                              // NB: do not use anything calling GDI!
     {
       mutable_graph_t *g = va_arg(va, mutable_graph_t *);
       int node           = va_arg(va, int);
       const char **text  = va_arg(va, const char **);
       bgcolor_t *bgcolor = va_arg(va, bgcolor_t *);
       *text = data->getBasicBlockText(node);
       if ( bgcolor != NULL )
         *bgcolor = DEFCOLOR;
       result = true;
       qnotused(g);
     }
     break;


    case grcode_user_size:    // calculate node size for user-defined graph
                              // in:  mutable_graph_t *g
                              //      int node
                              //      int *cx
                              //      int *cy
                              // out: 0-did not calculate, ida will use node text size
                              //      1-calculated. ida will add node title to the size
     msg("calc node size - not implemented\n");
     // ida will calculate the node size based on the node text
     break;

    case grcode_user_title:   // render node title of a user-defined graph
                              // in:  mutable_graph_t *g
                              //      int node
                              //      rect_t *title_rect
                              //      int title_bg_color
                              //      HDC dc
                              // out: 0-did not render, ida will fill it with title_bg_color
                              //      1-rendered node title
     // ida will draw the node title itself
     break;

    case grcode_user_draw:    // render node of a user-defined graph
                              // in:  mutable_graph_t *g
                              //      int node
                              //      rect_t *node_rect
                              //      HDC dc
                              // out: 0-not rendered, 1-rendered
                              // NB: draw only on the specified DC and nowhere else!
     // ida will draw the node text itself
     break;

    case grcode_user_hint:    // retrieve hint for the user-defined graph
                              // in:  mutable_graph_t *g
                              //      int mousenode
                              //      int mouseedge_src
                              //      int mouseedge_dst
                              //      char **hint
                              // 'hint' must be allocated by qalloc() or qstrdup()
                              // out: 0-use default hint, 1-use proposed hint
     {
       mutable_graph_t *g = va_arg(va, mutable_graph_t *);
       int mousenode      = va_argi(va, int);
       int mouseedge_src  = va_argi(va, int);
       int mouseedge_dst  = va_argi(va, int);
       char **hint        = va_arg(va, char **);
       char buf[MAXSTR];
       buf[0] = '\0';
       if ( mousenode != -1 )
         qsnprintf(buf, sizeof(buf), "My fancy hint for node %d", mousenode);
       else if ( mouseedge_src != -1 )
         qsnprintf(buf, sizeof(buf), "Hovering on (%d,%d)", mouseedge_src, mouseedge_dst);
       if ( buf[0] != '\0' )
         *hint = qstrdup(buf);
       result = true; // use our hint
       qnotused(g);
     }
     break;
  }
  return result;
}

static int idaapi on_ui_notification(void *, int code, va_list va)
{
    switch ( code )
    {
        case ui_ready_to_run:
            break;
        case ui_database_inited:
            break;
        case ui_term:
            break;
        case ui_plugin_loaded: {
            plugin_info_t* plugin_info = va_arg(va, plugin_info_t*);
            if (plugin_info && strcmp(plugin_info->name, "IDAPython") == 0) {
                plugin_init_python();
            }
            break;
        }
    case ui_plugin_unloading:
      break;
    default:
      break;
  }
  return 0;
}

//--------------------------------------------------------------------------
bool idaapi menu_callback(void *ud)
{
  graph_viewer_t *gv = (graph_viewer_t *)ud;
  mutable_graph_t *g = get_viewer_graph(gv);
  int code = askbuttons_c("Circle", "Tree", "Digraph", 1, "Please select layout type");
  node_info_t ni;
  ni.bg_color = 0x44FF55;
  ni.text = "Hello from plugin!";
  set_node_info2(g->gid, 7, ni, NIF_BG_COLOR | NIF_TEXT);
  g->current_layout = code + 2;
  g->circle_center = point_t(200, 200);
  g->circle_radius = 200;
  g->redo_layout();
  refresh_viewer(gv);
  return true;
}

static uint64_t ida_load_code(void *env, uint64_t ptr, uint32_t memop, uint32_t idx)
{
    if (!getFlags(ptr)) {
        Libqemu_RaiseError(env, 0xdeadbeef);
    }

    switch(memop & LQ_MO_SIZE)
    {
        case LQ_MO_8:
            return get_byte(ptr);
        case LQ_MO_16:
            return get_word(ptr);
        case LQ_MO_32:
            return get_long(ptr);
        case LQ_MO_64:
            return get_qword(ptr);
    }

    assert(false && "Should not arrive here");
}

//--------------------------------------------------------------------------
int idaapi PLUGIN_init(void)
{
    const char* so_name = NULL;
    processor_info = ida_get_processor_information();

    int err = Libqemu_Load(processor_info.processor);
    if (err || !Libqemu_Init) {
        llvm::errs() << "Error initializing libqemu library" << '\n';
        return PLUGIN_SKIP;
    }

    Libqemu_Init(ida_load_code, NULL);

    //If python plugin is already loaded, run python initialization, otherwise hook
    //plugin initializations and run it when plugin is loaded.
    for (plugin_info_t* plugin_info = get_plugins(); plugin_info; plugin_info = plugin_info->next) {
        if (strcmp(plugin_info->name, "IDAPython") == 0) {
            plugin_init_python();
        }
    }
    hook_to_notification_point(HT_UI, on_ui_notification, NULL);

    functionPassManager = new FunctionPassManager(llvm::unwrap(Libqemu_GetModule()));
    functionPassManager->add(createInstructionNamerPass());
  
    return ida_is_graphical_mode() ? PLUGIN_KEEP : PLUGIN_SKIP;
}

//--------------------------------------------------------------------------
void idaapi PLUGIN_term(void)
{
    plugin_unload_python();
    Libqemu_Unload();
}

//--------------------------------------------------------------------------
void idaapi PLUGIN_run(int /*arg*/)
{
    ea_t screen_ea = get_screen_ea();
    llvm::Function* function = translate_function_to_llvm(get_screen_ea());
    if (function) {
        std::string insts_text;
        llvm::raw_string_ostream ss(insts_text);

        ss << *function;
        msg("LLVM: %s\n", ss.str().c_str());
    }

   
    HWND hwnd = NULL;
    TForm *form = create_tform("LLVM", &hwnd);
    if ( hwnd != NULL )
    {
        // get a unique graph id
        netnode id;
        id.create("$ ugraph sample");
        graph_viewer_t *gv = create_graph_viewer(form,  id, callback, new IDALLVMFunction(function), 0);
        open_tform(form, FORM_TAB|FORM_MENU|FORM_QWIDGET);
        if ( gv != NULL )
        {
            mutable_graph_t *g = get_viewer_graph(gv);
            g->current_layout = layout_digraph;
            g->redo_layout();
            refresh_viewer(gv);
            viewer_center_on(gv, 0);
            //viewer_fit_window(gv);
//            viewer_add_menu_item(gv, "User function", menu_callback, gv, NULL, 0);
        }
    }
    else
    {
        close_tform(form, 0);
    }

}

//--------------------------------------------------------------------------
static const char PLUGIN_comment[] = PLUGIN_COMMENT;

static const char PLUGIN_help[] = PLUGIN_HELP;

//--------------------------------------------------------------------------
// This is the preferred name of the plugin module in the menu system
// The preferred name may be overriden in plugins.cfg file

static const char PLUGIN_wanted_name[] = PLUGIN_WANTED_NAME;


// This is the preferred hotkey for the plugin module
// The preferred hotkey may be overriden in plugins.cfg file
// Note: IDA won't tell you if the hotkey is not correct
//       It will just disable the hotkey.

static const char PLUGIN_wanted_hotkey[] = PLUGIN_HOTKEY;


//--------------------------------------------------------------------------
//
//      PLUGIN DESCRIPTION BLOCK
//
//--------------------------------------------------------------------------
plugin_t PLUGIN =
{
  IDP_INTERFACE_VERSION,
  0,                    // plugin flags
  PLUGIN_init,          // initialize

  PLUGIN_term,          // terminate. this pointer may be NULL.

  PLUGIN_run,           // invoke plugin

  PLUGIN_comment,       // long comment about the plugin
                        // it could appear in the status line
                        // or as a hint

  PLUGIN_help,          // multiline help about the plugin

  PLUGIN_wanted_name,   // the preferred short name of the plugin
  PLUGIN_wanted_hotkey  // the preferred hotkey to run the plugin
};
