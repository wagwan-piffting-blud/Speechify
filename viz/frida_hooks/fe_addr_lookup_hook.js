'use strict';
/* fe_addr_lookup_hook.js — identify what's at the addresses found in
 * the ctrl-block diff (real Speechify vs our hosted FE).
 * One-shot: dumps module info for a hard-coded list of addresses,
 * sends a single message, exits. */

var ADDRS = [
    /* The "func" half of every (data, func) pair installed by
     * setPair_E..H + installHookA/B in real Speechify */
    ptr('0x71f00806'),
    /* The "data" half - 6 different entries */
    ptr('0xb04c6000'),
    ptr('0xb04ca003'),
    ptr('0xb04cf003'),
    ptr('0xb04d4003'),
    ptr('0xb04c1003'),
    ptr('0xb04c4003'),
];

function info(addr) {
    var r = null;
    try { r = Process.findRangeByAddress(addr); } catch (e) {}
    var mod = null;
    try { mod = Process.findModuleByAddress(addr); } catch (e) {}
    var sym = null;
    try { sym = DebugSymbol.fromAddress(addr); } catch (e) {}
    return {
        addr: addr.toString(),
        range_protection: r ? r.protection : null,
        range_base: r ? r.base.toString() : null,
        range_size: r ? r.size : null,
        module_name: mod ? mod.name : null,
        module_base: mod ? mod.base.toString() : null,
        offset_in_module: mod ? '+0x' + addr.sub(mod.base).toString(16) : null,
        symbol: sym ? (sym.name || sym.toString()) : null,
    };
}

var out = ADDRS.map(info);
send({ type: 'addr_info', results: out });
send({ type: 'ready', hook: 'fe_addr_lookup' });
