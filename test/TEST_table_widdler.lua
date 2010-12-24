package.path = package.path .. ";;test/?.lua"
require "is_external"

require "socket"

local c   = 200;
local req = 10000000;
local mod = 100;

--local req = 1000000;

function init_ten_mill_mod100()
    local tbl  = "ten_mill_mod100";
    local indx = "ind_ten_mill_mod100_fk";
    drop_table(tbl);
    drop_index(indx);
    create_table(tbl, "id INT, fk INT, i INT");
    create_index(indx, tbl, 'fk');
    local icmd = '../gen-benchmark -q -c ' .. c ..' -n ' .. req ..
                 ' -s -m ' .. mod .. ' -A OK ' .. 
                 ' -Q INSERT INTO ten_mill_mod100 VALUES ' .. 
                 '"(000000000001,000000000001,1)" > /dev/null';
    os.execute(icmd);
    return "+OK";
end

function widdle_pk()
    local tbl  = "ten_mill_mod100";
    local cnt = scanselect("COUNT(*)", tbl);
    --print ('initial count: ' .. cnt);
    while (cnt > 0) do
        math.randomseed(socket.gettime()*10000)
        local res = scanselect("id", tbl, "ORDER BY id LIMIT 1");
        local pks = res[1];
        local r   = math.floor(math.random() * cnt);
        local pke = pks + r;
        local wc  = 'id BETWEEN ' .. pks .. ' AND ' .. pke;
        --print ('cnt: ' .. cnt .. ' r: ' .. r .. ' pks: ' .. pks ..
               --' pke: ' .. pke .. ' wc: ' .. wc);
        delete(tbl, wc);
        local new_cnt = cnt - (pke - pks) - 1;
        cnt = scanselect("COUNT(*)", tbl);
        if (cnt ~= new_cnt) then
            print ('expected: ' .. new_cnt .. ' got: ' .. cnt);
        end
    end
end

function widdle_FK()
    local tbl        = "ten_mill_mod100";
    local cnt        = scanselect("COUNT(*)", tbl);
    local cnt_per_fk = math.floor(cnt / mod);
    -- cnt never == req, Btree not 100% balanced, some FKs have more PKs
    local variance   = (cnt - req) / 10;
    --print ('cnt_per_fk: ' .. cnt_per_fk .. ' variance: ' .. variance);
    while (cnt > 0) do
        local res = scanselect("fk", tbl, "ORDER BY fk LIMIT 1");
        local fks = res[1];
        local r   = math.floor(math.random() * 10);
        local fke = fks + r;
        if (fke > mod) then
            fke = mod;
        end
        local wc  = 'fk BETWEEN ' .. fks .. ' AND ' .. fke;
        --print ('cnt: ' .. cnt .. ' r: ' .. r .. ' fks: ' .. fks ..
               --' fke: ' .. fke .. ' wc: ' .. wc);
        delete(tbl, wc);
        local new_cnt = cnt - (((fke - fks) + 1) * cnt_per_fk);
        cnt = scanselect("COUNT(*)", tbl);
        if ((new_cnt - cnt) > variance) then
            print ('expected: ' .. new_cnt .. ' got: ' .. cnt);
        end
    end
end

function run_widdler_test()
    init_ten_mill_mod100();
    widdle_pk();
    init_ten_mill_mod100();
    widdle_FK();
    return "+OK";
end

if is_external.yes == 1 then
    print (run_widdler_test());
end