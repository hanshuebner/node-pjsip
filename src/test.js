
var pjsua = require('./build/default/pjsip.node');
var repl = require('repl');

function processEvent(event, arg)
{
    console.log('got event', event, arg);
}

pjsua.start(processEvent);

pjsua.addAccount(process.env.SIP_USER, process.env.SIP_GATEWAY, process.env.SIP_PASSWORD);

var r = repl.start('node> ');

r.context.pjsua = pjsua;

r.context.done = function()
{
    pjsua.stop();

    process.exit();
};

