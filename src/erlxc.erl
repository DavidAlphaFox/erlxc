%%% Copyright (c) 2013-2014, Michael Santos <michael.santos@gmail.com>
%%% Permission to use, copy, modify, and/or distribute this software for any
%%% purpose with or without fee is hereby granted, provided that the above
%%% copyright notice and this permission notice appear in all copies.
%%%
%%% THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
%%% WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
%%% MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
%%% ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
%%% WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
%%% ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
%%% OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
-module(erlxc).
-include_lib("erlxc/include/erlxc.hrl").

-export([
        spawn/0, spawn/1, spawn/2,

        type/1,
        temporary/1, transitory/1, permanent/1,

        send/2,
        exit/2,

        container/1,
        console/1
    ]).

-type container() :: #container{port::port(),console::port()}.

-spec spawn() -> container().
-spec spawn(string() | binary()) -> container().
-spec spawn(string() | binary(),list()) -> container().
spawn() ->
    erlxc:spawn(<<>>, []).
spawn(Name) ->
    erlxc:spawn(Name, []).
spawn(<<>>, Options) ->
    erlxc:spawn(name(<<"erlxc">>), Options ++ [temporary]);
spawn(Name, Options) ->
    Port = erlxc_drv:start(Name, Options ++ [transitory]),
    boot(#container{port = Port}, Options).

-spec send(container(),iodata()) -> 'true'.
send(#container{console = Console}, Data) ->
    erlxc_console:send(Console, Data).

-spec exit(container(),'kill' | 'normal') -> boolean().
exit(#container{port = Port}, normal) ->
    liblxc:shutdown(Port, 0);

exit(#container{port = Port}, kill) ->
    liblxc:stop(Port).

type(#container{port = Port}) ->
    liblxc:type(Port).

temporary(#container{port = Port}) ->
    liblxc:temporary(Port).

transitory(#container{port = Port}) ->
    liblxc:transitory(Port).

permanent(#container{port = Port}) ->
    liblxc:permanent(Port).

container(#container{port = Port}) -> Port.
console(#container{console = Port}) -> Port.

%%--------------------------------------------------------------------
%%% Container state
%%--------------------------------------------------------------------

% "STOPPED", "STARTING", "RUNNING", "STOPPING",
% "ABORTING", "FREEZING", "FROZEN", "THAWED",
boot(#container{port = Port} = Container, Options) ->
    true = liblxc:async_state_notify(Port, 5),
    state(Container, Options).

state(#container{port = Port} = Container, Options) ->
    state(Container, liblxc:defined(Port), Options).
state(#container{port = Port} = Container, false, Options) ->
    case erlxc_drv:event(Port, infinity) of
        {state, <<"STOPPED">>} ->
            create(Container, Options),
            config(Container, Options),
            start(Container, Options),
            state(Container, Options);
        {state, State} ->
            erlang:error({error, State})
    end;
state(#container{port = Port} = Container, true, Options) ->
    case erlxc_drv:event(Port, infinity) of
        {state, <<"RUNNING">>} ->
            true = liblxc:async_state_close(Port),
            Name = liblxc:name(Port),
            Console = erlxc_console:start(Name),
            Container#container{console = Console};
        {state, <<"STOPPED">>} ->
            config(Container, Options),
            start(Container, Options),
            state(Container, Options);
        {state, <<"FROZEN">>} ->
            true = liblxc:unfreeze(Port),
            state(Container, true, Options);
        {state, State} when State =:= <<"STARTING">>; State =:= <<"STOPPING">>;
                State =:= <<"FREEZING">>; State =:= <<"THAWED">> ->
            state(Container, Options);
        {state, State} ->
            erlang:error({unsupported, State, Options})
    end.

config(#container{port = Port}, Options) ->
    Config = proplists:get_value(config, Options, []),

    [ begin
        case Item of
            <<>> ->
                true = liblxc:clear_config(Port);
            {Key, Value} ->
                true = liblxc:set_config_item(Port, Key, Value);
            Key ->
                true = liblxc:clear_config_item(Port, Key)
        end
      end || Item <- Config ].

create(#container{port = Port}, Options) ->
    Create = proplists:get_value(create, Options, []),

    Template = proplists:get_value(template, Create, <<"ubuntu">>),
    Bdevtype = proplists:get_value(bdevtype, Create, <<>>),
    Bdevspec = proplists:get_value(bdevspec, Create, <<>>),
    Flags = proplists:get_value(flags, Create, 0),
    Argv = proplists:get_value(argv, Create, []),

    true = liblxc:create(Port, Template, Bdevtype, Bdevspec, Flags, Argv).

start(#container{port = Port}, Options) ->
    Start = proplists:get_value(start, Options, []),
    UseInit = proplists:get_value(useinit, Start, false),
    Argv = proplists:get_value(argv, Start, []),

    true = liblxc:start(Port, bool(UseInit), Argv).

%%--------------------------------------------------------------------
%%% Internal functions
%%--------------------------------------------------------------------
bool(true) -> 1;
bool(false) -> 0;
bool(1) -> 1;
bool(0) -> 0.

i2b(N) ->
    list_to_binary(integer_to_list(N)).

name(Name) ->
    % XXX possible to re-use container names
    N = binary:decode_unsigned(crypto:rand_bytes(4)),
    <<Name/binary, (i2b(N))/binary>>.
