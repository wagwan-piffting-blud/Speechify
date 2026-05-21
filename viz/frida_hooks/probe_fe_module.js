'use strict';
/* One-shot diagnostic — find SWIttsFe / SWIttsEngine / SWIttsUSel module
 * loads in the target process, report base addresses. */
var mods_of_interest = [
    'SWIttsFe-en-US.dll',
    'SWIttsEngine.dll',
    'SWIttsUSel.dll',
    'SWIttsFe.dll',
];
var found = {};
var all_names = [];
Process.enumerateModules().forEach(function (m) {
    all_names.push(m.name);
    for (var i = 0; i < mods_of_interest.length; i++) {
        if (m.name.toLowerCase() === mods_of_interest[i].toLowerCase()) {
            found[m.name] = { base: m.base.toString(), size: m.size };
        }
    }
});
send({ type: 'module_probe', found: found, n_modules: all_names.length });
// Send list of dll-suffixed module names for visibility
var dlls = all_names.filter(function (n) { return /\.dll$/i.test(n); });
send({ type: 'module_probe_dlls', dlls: dlls.slice(0, 200) });
send({ type: 'ready', hook: 'probe_fe_module' });
