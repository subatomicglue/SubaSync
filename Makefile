all: release debug
	file build/*/sync
release: ./build/Release/sync
debug: ./build/Debug/sync

establish_dirs:
	rm -rf build/Release/sandbox1 && mkdir -p build/Release/sandbox1/.config && mkdir -p build/Release/sandbox1/share/
	rm -rf build/Release/sandbox2 && mkdir -p build/Release/sandbox2/.config && mkdir -p build/Release/sandbox2/share/
	rm -rf build/Release/sandbox3 && mkdir -p build/Release/sandbox3/.config && mkdir -p build/Release/sandbox3/share/
	ln -s `pwd`/sandbox1-settings.json build/Release/sandbox1/.config/settings.json
	ln -s `pwd`/sandbox2-settings.json build/Release/sandbox2/.config/settings.json
	ln -s `pwd`/sandbox3-settings.json build/Release/sandbox3/.config/settings.json
	ln -s `pwd`/sandbox1-watches.json build/Release/sandbox1/.config/watches.json
	ln -s `pwd`/sandbox2-watches.json build/Release/sandbox2/.config/watches.json
	ln -s `pwd`/sandbox3-watches.json build/Release/sandbox3/.config/watches.json
	ln -s `pwd`/sandbox1-dir-guids.json build/Release/sandbox1/.config/dir-guids.json
	ln -s `pwd`/sandbox2-dir-guids.json build/Release/sandbox2/.config/dir-guids.json
	ln -s `pwd`/sandbox3-dir-guids.json build/Release/sandbox3/.config/dir-guids.json
	cp SUMMARY.md build/Release/sandbox1/share/
	cp README.md build/Release/sandbox2/share/
	mkdir -p build/Release/sandbox1/share/myfiles
	mkdir -p build/Release/sandbox2/share/mystuff
	cp CMakeLists.txt build/Release/sandbox1/share/myfiles/
	cp conanfile.txt build/Release/sandbox2/share/mystuff/

build_the_thing:
	mkdir -p build
	$(MAKE) establish_dirs
	cd build && conan install .. --build=missing -s compiler.cppstd=17 -s build_type=${BUILD_TYPE} # will generate conanbuildinfo.cmake
	cd build && cmake -B ${BUILD_TYPE} -S .. -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake
	cd build && cmake --build ${BUILD_TYPE} --config ${BUILD_TYPE} -j8
	ls build/*/sync

./build/Debug/sync: src/*.cpp src/*.hpp
	$(MAKE) build_the_thing BUILD_TYPE=Debug

./build/Release/sync: src/*.cpp src/*.hpp
	$(MAKE) build_the_thing BUILD_TYPE=Release

# run the xcode apple debugger
# depends on ./build/Debug/sync being made, if not then make it
lldb: ./build/Debug/sync
	@echo "======================================================="
	@echo "To debug, run:"
	@echo "run 9000 127.0.0.1 \"\" peerA"
	@echo "Then, in another terminal, run:"
	@echo "run 9001 127.0.0.1 127.0.0.1:9000 peerB"
	@echo "======================================================="
	@echo "Useful lldb commands:"
	@echo "Command                                  Description"
	@echo "---------------------------------------- --------------------------------"
	@echo "settings set target.run-args <args>      Set program arguments"
	@echo "run                                      Start program execution"
	@echo "process continue                         Continue running after breakpoint"
	@echo "c                                        Short form"
	@echo "process interrupt                        Pause execution (Ctrl+C also works)"
	@echo "process kill                             Stop program"
	@echo ""
	@echo "breakpoint set --name main               Break at main()"
	@echo "b main                                   Short form"
	@echo "b Connection::connect_outgoing           Break at class method"
	@echo "b PeerManager::attempt_connect_more      Break at class method"
	@echo "b 42                                     Break at line 42"
	@echo "b myfile.cpp:42                          Specific file + line"
	@echo "breakpoint list                          List all breakpoints"
	@echo "breakpoint delete 1                      Delete breakpoint #1"
	@echo ""
	@echo "step                                     Step into function"
	@echo "s                                        Short form"
	@echo "next                                     Step over line"
	@echo "n                                        Short form"
	@echo "finish                                   Run until current function returns"
	@echo "thread step-out                          Alias for finish"
	@echo ""
	@echo "frame variable                           Show all local variables in current frame"
	@echo "fr v                                     Short form"
	@echo "fr v my_var                              Show specific variable"
	@echo "expr my_var                              Evaluate expression or modify value"
	@echo "expr my_var = 42                         Modify value"
	@echo "target variable my_var                   Another way to print variable"
	@echo ""
	@echo "thread list                              Show all threads"
	@echo "thread select 2                          Switch to thread #2"
	@echo "bt                                       Backtrace current thread"
	@echo "thread backtrace                         Same as bt"
	@echo "up / down                                Move up/down stack frames"
	@echo ""
	@echo "watchpoint set variable my_var           Trigger when variable changes"
	@echo "watchpoint list                          Show watchpoints"
	@echo "watchpoint delete 1                      Delete watchpoint #1"
	@echo ""
	@echo "image lookup -n connect_outgoing         Find function addresses"
	@echo "disassemble --name main                  Disassemble function"
	@echo "disassemble --pc                         Disassemble current PC"
	@echo ""
	@echo "help                                     List LLDB commands"
	@echo "help run                                 Help for run command"
	@echo "settings show                            Show settings"
	@echo "quit                                     Exit LLDB"
	@/usr/bin/env lldb -o run ./build/Debug/sync -- 9000 127.0.0.1 \"\" peerA

# run the gnu debugger
# depends on ./build/Debug/sync being made, if not then make it
gdb: ./build/Debug/sync
	@echo "======================================================="
	@echo "To debug, run:"
	@echo "set args 9000 127.0.0.1 \"\" peerA"
	@echo "run"
	@echo "Then, in another terminal, run:"
	@echo "set args 9001 127.0.0.1 127.0.0.1:9000 peerB"
	@echo "run"
	@echo "======================================================="
	@echo "Useful gdb commands:"
	@echo "Command                                  Description"
	@echo "---------------------------------------- --------------------------------"
	@echo "set args <args>                          Set program arguments"
	@echo "run                                      Start program execution"
	@echo "start                                    Start program execution and break at main() entrypoint"
	@echo "attach <pid>                             attach to running process"
	@echo "continue or c                            Continue running after breakpoint"
	@echo "break or b                               Short form"
	@echo "ctrl-c or interrupt                      Pause execution"
	@echo "kill                                     Stop program"
	@echo ""
	@echo "bt                                       Show stack trace (backtrace)"
	@echo "break main                               Set breakpoint at main()"
	@echo "break Connection::connect_outgoing       Break when outgoing connection starts"
	@echo "break PeerManager::attempt_connect_more  Break on periodic peer connect"
	@echo "next or n                                Step over one line"
	@echo "step or s                                Step into function"
	@echo "print <var>                              Print value of variable"
	@echo "info locals                              Show local variables"
	@echo "frame variable (fr v)                    Show variables in current frame"
	@echo "set pagination off                       Disable 'more' pauses in output"
	@echo "quit                                     Exit GDB"
	gdb -ex "run 9000 127.0.0.1 peerA" ./build/Debug/sync

# run this on macos to sign your gdb binary so it can control other processes
# NOTE:  I never did get this working!  :(
#        spctl --assess --type execute `which gdb`.... gives "rejected"
gdb-codesign:
	@echo "This is a one-time setup."
	@echo "1.)"
	@echo "In order to sign your gdb, so it can control other processes"
	@echo "We will launch 'Keychain Access' for you.  Follow the directions below to create a self-signed cert for code signing."
	@echo " - Menu: Keychain Access → Certificate Assistant → Create a Certificate..."
	@echo " - Name: gdb-cert"
	@echo " - Identity Type: Self-Signed Root"
	@echo " - Certificate Type: Code Signing"
	@echo " - Check Let me override defaults → Continue with defaults....    Key Usage → Sign Code: ✅"
	@echo " - Save to login keychain"
	@echo " - In System Keychain / System Roots, right-click certificate → Get Info → Trust → When using this certificate → Always Trust"
	@read -p "hit Enter to Continue (or CTRL-C to abort)"
	@security find-certificate -c "gdb-cert" &>/dev/null && (echo " - gdb-cert found, skipping Keychain Access... (if you need to edit, run:    open -a \"Keychain Access\")")
	@security find-certificate -c "gdb-cert" &>/dev/null || (echo " - gdb-cert not found, opening Keychain Access..."; open -a "Keychain Access")
	@read -p "hit Enter to Continue (or CTRL-C to abort)"
	@echo "2.)"
	@echo "your gdb is currently signed like this:"
	codesign -dv --verbose=4 `which gdb`
	@echo ""
	@echo "If unsigned, you should see:"
	@echo " - Signature=adhoc"
	@echo " - TeamIdentifier=not set"
	@echo "If signed, you should see:"
	@echo " - Authority=gdb-cert"
	@echo "3.)"
	@read -p "Ready to sign?  hit Enter to Continue (or CTRL-C to abort)"
	sudo codesign --force --verify --verbose --sign "gdb-cert" `which gdb` --entitlements gdb-debug.entitlements
	@echo "4.)"
	@read -p "hit Enter to Continue (or CTRL-C to abort)"
	@echo "your gdb is currently signed like this:"
	codesign -dv --verbose=4 `which gdb`
	@echo ""
	@echo "If unsigned, you should see:"
	@echo " - Signature=adhoc"
	@echo " - TeamIdentifier=not set"
	@echo "If signed, you should see:"
	@echo " - Authority=gdb-cert"
	@echo "5.)"
	@read -p "RESTART taskgated & verify....      hit Enter to Continue (or CTRL-C to abort)"
	sudo killall -9 taskgated
	sudo xattr -dr com.apple.quarantine `which gdb`
	codesign -vvv --verify --deep --strict `which gdb`
	spctl --assess --type execute `which gdb`
	ls -lO `which gdb`


gdb-codesign-reset:
	sudo security delete-certificate -c gdb-cert /Library/Keychains/System.keychain
	security delete-certificate -c gdb-cert ~/Library/Keychains/login.keychain-db

# AUTOMATIC
# you may need to do this if you're building x86_64 by default, but you're on apple silicon
conan-setup:
	conan profile detect --force
	conan profile show

# FORCE TO ARM64 (avoid this if possible)
# you may need to do this if you're building x86_64 by default, but you're on apple silicon
# gdb's architecture, must match, your executable architecture
conan-arm-profile:
	@echo "[settings]" > ~/.conan2/profiles/default
	@echo "arch=arm64" >> ~/.conan2/profiles/default
	@echo "build_type=Release" >> ~/.conan2/profiles/default
	@echo "compiler=apple-clang" >> ~/.conan2/profiles/default
	@echo "compiler.cppstd=gnu17" >> ~/.conan2/profiles/default
	@echo "compiler.libcxx=libc++" >> ~/.conan2/profiles/default
	@echo "compiler.version=17" >> ~/.conan2/profiles/default
	@echo "os=Macos" >> ~/.conan2/profiles/default
	conan profile show

# FORCE TO INTEL (avoid this if possible)
# you may need to do this if you're building x86_64 by default, but you're on apple silicon
# gdb's architecture, must match, your executable architecture
conan-intel-profile:
	@echo "[settings]" > ~/.conan2/profiles/default
	@echo "arch=x86_64" >> ~/.conan2/profiles/default
	@echo "build_type=Release" >> ~/.conan2/profiles/default
	@echo "compiler=apple-clang" >> ~/.conan2/profiles/default
	@echo "compiler.cppstd=gnu17" >> ~/.conan2/profiles/default
	@echo "compiler.libcxx=libc++" >> ~/.conan2/profiles/default
	@echo "compiler.version=17" >> ~/.conan2/profiles/default
	@echo "os=Macos" >> ~/.conan2/profiles/default
	conan profile show

clean:
	rm -rf build
