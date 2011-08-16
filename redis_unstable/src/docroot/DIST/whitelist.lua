dofile "./docroot/DIST/short_stack.lua";
-- Retwis for Alchemy's Short Stack - PUBLIC API

function I_index_page(start) 
  if (CheckEtag('index_page')) then   return;                        end
  if (CacheExists('index_page')) then return CacheGet('index_page'); end
  local my_userid;
  if (LoggedIn) then my_userid = MyUserid;
  else               my_userid = getrand();  end
  init_output();
  create_header(my_userid);
  create_welcome();
  create_footer();
  CachePutOutput('index_page');
  return flush_output();
end
function WL_index_page(start)
  if (isLoggedIn()) then SetHttpRedirect('/home'); return; end
  return I_index_page(start);
end

function I_home(my_userid, my_username, s)
  setIsLoggedIn(my_userid); -- used for internal redirects
  local nflwers   = redis("scard", "uid:" .. my_userid .. ":followers");
  local nflwing   = redis("scard", "uid:" .. my_userid .. ":following");
  local nposts    = redis("llen",  "uid:" .. my_userid .. ":posts");
  local my_userid = MyUserid;
  if (CheckEtag('home', my_userid, nposts, nflwers, nflwing)) then return; end
  init_output();
  create_header(my_userid);
  create_home(my_userid, my_username, s, nposts, nflwers, nflwing);
  create_footer();
  return flush_output();
end
function WL_home(s)
  if (isLoggedIn() == false) then
    SetHttpRedirect(build_link(getrand(), 'index_page')); return;
  else
    local my_userid   = MyUserid;
    if (IsCorrectNode(my_userid) == false) then -- home ONLY to shard-node
      SetHttpRedirect(build_link(my_userid, 'index_page')); return;
    end
    local my_username = redis("get", "uid:" .. my_userid .. ":username");
    return I_home(my_userid, my_username, s);
  end
end

function I_profile(userid, username, s)
  local page   = '/profile';
  local isl    = isLoggedIn(); -- populates MyUserid
  local my_userid;
  if (LoggedIn) then my_userid = MyUserid;
  else               my_userid = getrand();  end
  local nposts = redis("llen", "uid:" .. userid .. ":myposts")
  local f      = -1;
  if (isl and my_userid ~= userid) then
    local isf = redis("sismember", "uid:" .. my_userid .. ":following", userid);
    if (isf == 1) then f = 1;
    else               f = 0; end
  end
  SetHttpResponseHeader('Set-Cookie', 'following=' .. f ..
                                      '; Max-Age=1; path=/;');
  SetHttpResponseHeader('Set-Cookie', 'otheruser=' .. userid ..
                                      '; Max-Age=1; path=/;');
  if (CheckEtag('profile', isl, userid, nposts, s)) then return; end
  s = tonumber(s);
  if (s == nil) then s = 0; end
  if (s == 0) then -- CACHE only 1st page
    if (CacheExists('profile', isl, userid, nposts)) then
      return CacheGet('profile', isl, userid, nposts); end end

  init_output();
  create_header(my_userid);
  output("<h2 class=\"username\">" .. username .. "</h2>");
  create_follow();
  showUserPostsWithPagination(page, nposts, username, userid, s, 10);
  create_footer();
  CachePutOutput('profile', isl, userid, nposts);
  return flush_output();
end
function WL_profile(userid, start)
  if (is_empty(userid)) then
    SetHttpRedirect(build_link(getrand(), 'index_page')); return;
  end
  if (IsCorrectNode(userid) == false) then -- profile ONLY to shard-node
    SetHttpRedirect(build_link(userid, 'profile', userid, start)); return;
  end
  local username = redis("get", "uid:" .. userid .. ":username");
  if (username == nil) then -- FOR: hackers doing userid scanning
    SetHttpRedirect(build_link(getrand(), 'index_page')); return;
  end
  return I_profile(userid, username, start);
end

function WL_follow(muserid, userid, follow) -- muserid used ONLY by haproxy
  if (is_empty(userid) or is_empty(follow) or isLoggedIn() == false) then
    SetHttpRedirect(build_link(getrand(), 'index_page')); return;
  end
  local my_userid = MyUserid;
  if (IsCorrectNode(my_userid) == false) then -- follow ONLY to shard-node
    SetHttpRedirect(build_link(my_userid, 'follow', my_userid, userid, follow));
    return;
  end
  if (userid ~= my_userid) then
    call_sync(global_follow, 'global_follow', my_userid, userid, follow);
    local_follow(my_userid, userid, follow);
  end
  SetHttpRedirect(build_link(userid, 'profile', userid));
end

function WL_register(username, password)
  init_output();
  if (is_empty(username) or is_empty(password)) then
    goback(getrand(), "Username or Password is Empty"); return flush_output();
  end
  username         = url_decode(username);
  password         = url_decode(password);
  if (redis("get", "username:" .. username .. ":id")) then
    goback(getrand(), "Sorry the selected username is already in use.");
    return flush_output();
  end
  -- Everything is ok, Register the user!
  local my_userid  = IncrementAutoInc('NextUserId');
  local authsecret = call_sync(global_register, 'global_register',
                               my_userid,       username);
  local_register(my_userid, username, password);

  -- User registered -> Log him in
  SetHttpResponseHeader('Set-Cookie', 'auth=' .. authsecret .. 
                            '; Expires=Wed, 09 Jun 2021 10:18:14 GMT; path=/;');

  create_header(my_userid);
  output('<h2>Welcome aboard!</h2> Hey ' .. username ..
             ', now you have an account, <a href=' .. 
            build_link(my_userid, "index_page") .. 
             '>a good start is to write your first message!</a>.');
  create_footer();
  return flush_output();
end

function WL_logout(muserid) -- muserid used for URL haproxy LoadBalancing
  if (isLoggedIn() == false) then
    SetHttpRedirect(build_link(muserid, 'index_page')); return;
  end
  local my_userid     = MyUserid;
  if (IsCorrectNode(my_userid) == false) then -- logout BETTER at shard-node
    SetHttpRedirect(build_link(my_userid, 'logout', my_userid));
    return;
  end
  call_sync(global_logout, 'global_logout', my_userid);
  return I_index_page(0); -- internal redirect
end

function WL_login(o_username, o_password)
  init_output();
  if (is_empty(o_username) or is_empty(o_password)) then
    goback(getrand(), "Enter both username and password to login.");
    return flush_output();
  end
  local my_username  = url_decode(o_username);
  local password     = url_decode(o_password);
  local my_userid    = redis("get", "username:" .. my_username ..":id");
  if (my_userid == nil) then
    goback(getrand(), "Wrong username or password"); return flush_output();
  end
  if (IsCorrectNode(my_userid) == false) then -- login ONLY 2 shard-node
    SetHttpRedirect(build_link(my_userid, 'login', o_username, o_password));
    return;
  end
  local realpassword = redis("get", "uid:" .. my_userid .. ":password");
  if (realpassword ~= password) then
    goback(getrand(), "Wrong username or password"); return flush_output();
  end
  -- Username / password OK, set the cookie and internal redirect to home
  local authsecret   = redis("get", "uid:" .. my_userid .. ":auth");
  SetHttpResponseHeader('Set-Cookie', 'auth=' .. authsecret ..
                            '; Expires=Wed, 09 Jun 2021 10:18:14 GMT; path=/;');
  return I_home(my_userid, my_username, 0); -- internal redirect
end

function WL_post(muserid, o_msg) -- muserid used for URL haproxy LoadBalancing
  if (is_empty(o_msg) or isLoggedIn() == false) then
    SetHttpRedirect(build_link(muserid, 'index_page')); return;
  end
  local my_userid = MyUserid;
  if (IsCorrectNode(my_userid) == false) then -- post ONLY to shard-node
    SetHttpRedirect(build_link(my_userid, 'post', my_userid, o_msg)); return;
  end
  local msg         = url_decode(o_msg);
  local ts          = gettime();
  local postid      = IncrementAutoInc('NextPostId');
  call_sync(global_post, 'global_post', my_userid, postid, ts, msg);
  local_post(my_userid, postid, msg, ts);
  local my_username = redis("get", "uid:" .. my_userid .. ":username");
  return I_home(my_userid, my_username, 0); -- internal redirect
end

function WL_timeline()
  -- dependencies: n_global_users, n_global_timeline
  -- page is too volatile to cache -> NO CACHING
  local my_userid;
  if (isLoggedIn()) then my_userid = MyUserid;
  else                   my_userid = getrand();  end
  init_output();
  create_header(my_userid);
  showLastUsers();
  output('<i>Latest 20 messages from users aroud the world!</i><br>');
  showUserPosts("global:timeline", 0, 20);
  create_footer();
  return flush_output();
end

-- DEBUG DEBUG DEBUG DEBUG DEBUG DEBUG DEBUG DEBUG DEBUG DEBUG DEBUG DEBUG
function WL_hello_world()
  return 'HELLO WORLD';
end
redis("set", 'HELLO WORLD', 'HELLO WORLD');
function WL_hello_world_data()
  return redis("get", 'HELLO WORLD');
end
