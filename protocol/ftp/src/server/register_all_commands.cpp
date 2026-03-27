#include "server/command.h"
#include "server/commands/abort.h"
#include "server/commands/acct.h"
#include "server/commands/allo.h"
#include "server/commands/appe.h"
#include "server/commands/cdup.h"
#include "server/commands/cwd.h"
#include "server/commands/dele.h"
#include "server/commands/help.h"
#include "server/commands/list.h"
#include "server/commands/mkd.h"
#include "server/commands/mode.h"
#include "server/commands/nlist.h"
#include "server/commands/noop.h"
#include "server/commands/pass.h"
#include "server/commands/pasv.h"
#include "server/commands/port.h"
#include "server/commands/pwd.h"
#include "server/commands/quit.h"
#include "server/commands/rein.h"
#include "server/commands/rest.h"
#include "server/commands/retr.h"
#include "server/commands/rmd.h"
#include "server/commands/rnfr.h"
#include "server/commands/rnto.h"
#include "server/commands/site.h"
#include "server/commands/size.h"
#include "server/commands/stat.h"
#include "server/commands/stor.h"
#include "server/commands/stou.h"
#include "server/commands/stru.h"
#include "server/commands/syst.h"
#include "server/commands/type.h"
#include "server/commands/user.h"

namespace yuan::net::ftp
{
    void ensure_all_commands_registered()
    {
        static bool once = []() {
            auto factory = CommandFactory::get_instance();
            factory->register_command(new CommandAbort());
            factory->register_command(new CommandAcct());
            factory->register_command(new CommandAllo());
            factory->register_command(new CommandAppe());
            factory->register_command(new CommandCdup());
            factory->register_command(new CommandCwd());
            factory->register_command(new CommandDele());
            factory->register_command(new CommandHelp());
            factory->register_command(new CommandList());
            factory->register_command(new CommandMkd());
            factory->register_command(new CommandMode());
            factory->register_command(new CommandNlist());
            factory->register_command(new CommandNoop());
            factory->register_command(new CommandPass());
            factory->register_command(new CommandPasv());
            factory->register_command(new CommandPort());
            factory->register_command(new CommandPwd());
            factory->register_command(new CommandQuit());
            factory->register_command(new CommandRein());
            factory->register_command(new CommandRest());
            factory->register_command(new CommandRetr());
            factory->register_command(new CommandRmd());
            factory->register_command(new CommandRnfr());
            factory->register_command(new CommandRnto());
            factory->register_command(new CommandSite());
            factory->register_command(new CommandSize());
            factory->register_command(new CommandStat());
            factory->register_command(new CommandStor());
            factory->register_command(new CommandStou());
            factory->register_command(new CommandStru());
            factory->register_command(new CommandSyst());
            factory->register_command(new CommandTypeCmd());
            factory->register_command(new CommandUser());
            return true;
        }();
        (void)once;
    }
}
