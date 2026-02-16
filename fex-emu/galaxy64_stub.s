    .text

    # Return NULL stubs (xor rax,rax; ret)
    .macro null_stub name
    .globl \name
    .def \name; .scl 2; .type 32; .endef
\name:
    xorq %rax, %rax
    ret
    .endm

    # Void stubs (just ret)
    .macro void_stub name
    .globl \name
    .def \name; .scl 2; .type 32; .endef
\name:
    ret
    .endm

    # Pointer-returning accessors -> return NULL
    null_stub "?Apps@api@galaxy@@YAPEAVIApps@12@XZ"
    null_stub "?Chat@api@galaxy@@YAPEAVIChat@12@XZ"
    null_stub "?CustomNetworking@api@galaxy@@YAPEAVICustomNetworking@12@XZ"
    null_stub "?Friends@api@galaxy@@YAPEAVIFriends@12@XZ"
    null_stub "?GameServerListenerRegistrar@api@galaxy@@YAPEAVIListenerRegistrar@12@XZ"
    null_stub "?GameServerLogger@api@galaxy@@YAPEAVILogger@12@XZ"
    null_stub "?GameServerMatchmaking@api@galaxy@@YAPEAVIMatchmaking@12@XZ"
    null_stub "?GameServerNetworking@api@galaxy@@YAPEAVINetworking@12@XZ"
    null_stub "?GameServerUser@api@galaxy@@YAPEAVIUser@12@XZ"
    null_stub "?GameServerUtils@api@galaxy@@YAPEAVIUtils@12@XZ"
    null_stub "?GetError@api@galaxy@@YAPEBVIError@12@XZ"
    null_stub "?ListenerRegistrar@api@galaxy@@YAPEAVIListenerRegistrar@12@XZ"
    null_stub "?Logger@api@galaxy@@YAPEAVILogger@12@XZ"
    null_stub "?Matchmaking@api@galaxy@@YAPEAVIMatchmaking@12@XZ"
    null_stub "?Networking@api@galaxy@@YAPEAVINetworking@12@XZ"
    null_stub "?ServerNetworking@api@galaxy@@YAPEAVINetworking@12@XZ"
    null_stub "?Stats@api@galaxy@@YAPEAVIStats@12@XZ"
    null_stub "?Storage@api@galaxy@@YAPEAVIStorage@12@XZ"
    null_stub "?User@api@galaxy@@YAPEAVIUser@12@XZ"
    null_stub "?Utils@api@galaxy@@YAPEAVIUtils@12@XZ"
    null_stub load

    # Void functions -> no-op
    void_stub "?Init@api@galaxy@@YAXAEBUInitOptions@12@@Z"
    void_stub "?InitGameServer@api@galaxy@@YAXAEBUInitOptions@12@@Z"
    void_stub "?ProcessData@api@galaxy@@YAXXZ"
    void_stub "?ProcessGameServerData@api@galaxy@@YAXXZ"
    void_stub "?Shutdown@api@galaxy@@YAXXZ"
    void_stub "?ShutdownGameServer@api@galaxy@@YAXXZ"
