download_project()
{
	if [ ! -d "3rdparty" ]; then
        mkdir 3rdparty
    fi
	cd 3rdparty
	if [ ! -d $1 ]; then
	    git.exe clone --depth=1 $2 $1
    else
		cd $1
	    git pull
		cd ..
    fi
	cd ..
}

build_recast()
{
    ./genie --file=recastnavigation.lua gmake
    pushd 3rdparty/recast/_project
    make 
    popd
}

build_cmft()
{
   	pushd 3rdparty/cmft/scripts
	cp ../../../genie genie
	./genie --file=main.lua --gcc=linux-gcc gmake
	rm genie
	cd ../_projects/gmake-linux
    make config=release64 cmft
    cd ../../_build/linux64_gcc/bin
    ranlib libcmftRelease.a
	popd
}

build_luajit()
{
    pushd 3rdparty/luajit/src
    make
    popd
}

build_nvtt()
{
    pushd 3rdparty/nvtt
    chmod +x configure
    ./configure --release
    make
    popd
}

build_physx()
{
    pushd 3rdparty/physx/physx
    sh generate_projects.sh lumix_gcc5
    cd compiler/linux-release
    make
    popd
}

build_freetype() 
{
    pushd 3rdparty/freetype
    jam
    popd
}

pause()
{
    read -p "Press enter to continue"
}

build_3rdparty()
{
    case "$1" in
        "cmft" ) build_cmft;;
        "freetype" ) build_freetype;;
        "luajit" ) build_luajit;;
        "nvtt" ) build_nvtt;;
        "physx" ) build_physx;;
        "recast" ) build_recast;;
    esac
}

deploy_cmft()
{
    mkdir -p ../external/cmft/lib/linux64_gmake/release
    cp 3rdparty/cmft/_build/linux64_gcc/bin/libcmftRelease.a ../external/cmft/lib/linux64_gmake/release/libcmft.a
}

deploy_freetype()
{
    mkdir -p ../external/freetype/lib/linux64_gmake/release
    cp 3rdparty/freetype/objs/libfreetype.a ../external/freetype/lib/linux64_gmake/release/libfreetype.a
}

deploy_luajit()
{
    mkdir -p ../external/luajit/lib/linux64_gmake/release
    cp 3rdparty/luajit/src/libluajit.a ../external/luajit/lib/linux64_gmake/release/libluajit.a
}

deploy_nvtt()
{
    mkdir -p ../external/nvtt/lib/linux64_gmake/release
    cp 3rdparty/nvtt/build-release/src/nvtt/libnvtt.a ../external/nvtt/lib/linux64_gmake/release/libnvtt.a
}

deploy_recast()
{
    mkdir -p ../external/recast/lib/linux64_gmake/release
    cp 3rdparty/recast/_build/libRecast.a ../external/recast/lib/linux64_gmake/release/librecast.a
}

deploy_physx()
{
    mkdir -p ../external/physx/lib/linux64_gmake/release
    cp 3rdparty/physx/physx/bin/linux.clang/release/*.a ../external/physx/lib/linux64_gmake/release/
}

deploy_3rdparty()
{
    case "$1" in
        "cmft" ) deploy_cmft;;
        "freetype" ) deploy_freetype;;
        "luajit" ) deploy_luajit;;
        "nvtt" ) deploy_nvtt;;
        "physx" ) deploy_physx;;
        "recast" ) deploy_recast;;
    esac
}

project_menu()
{
    while :; do
        clear;
        echo $1
        options=("download", "build", "deploy", "back")
        select opt in "${options[@]}"
        do
            case "$REPLY" in
                1 ) download_project $1 $2; pause; break;;
                2 ) build_3rdparty $1; pause; break;;
                3 ) deploy_3rdparty $1; pause; break;;
                4 ) break 2;;
            esac
        done
    done
}

thirdparty_menu()
{
    while :; do
        clear;
        echo "third party"
        options=("cmft" "freetype" "luajit" "nvtt" "physx" "recast" "back")
        select opt in "${options[@]}"
        do
            case "$REPLY" in
                1 ) project_menu "cmft" "https://github.com/nem0/cmft.git"; break;;
                2 ) project_menu "freetype" "https://github.com/nem0/freetype2.git"; break;;
                3 ) project_menu "luajit" "https://github.com/nem0/LuaJIT.git"; break;;
                4 ) project_menu "nvtt" "https://github.com/nem0/nvidia-texture-tools.git"; break;;
                5 ) project_menu "physx" "https://github.com/nem0/PhysX.git"; break;;
                6 ) project_menu "recast" "https://github.com/nem0/recastnavigation.git"; break;;
                7 ) break 2;;
            esac
        done
    done
}

build()
{
    cd tmp/gmake
    make -j config=relwithdebinfo64
    cd ../..
}

main_menu()
{
    clear;
    PS3="Wut? "
    options=("create project", "build" "3rdparty", "exit")
    select opt in "${options[@]}"
    do
        case "$REPLY" in
            1 ) ./genie --static-plugins gmake; pause; break;;
            2 ) build; pause; break;;
            3 ) thirdparty_menu; break;;
            4 ) exit;;
        esac
    done
}

while :; do main_menu; done