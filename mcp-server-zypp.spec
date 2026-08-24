#
# spec file for package mcp-server-zypp
#
# Copyright (c) 2025 SUSE LLC
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (please see the
# file COPYING for the license details). There is no warranty for this good,
# even the implied warranty of merchantability or fitness for a particular
# purpose.
#
# Please submit bugfixes or comments via https://bugs.opensuse.org/
#

Name:           mcp-server-zypp
Version:        0.1.0
Release:        0
Summary:        MCP server exposing libzypp package management to LLM agents
License:        GPL-3.0-or-later
URL:            https://github.com/openSUSE/mcp-server-zypp
Source0:         %{name}-%{version}.tar.bz2
# Vendored Go dependencies for proxy/ — generated via: osc service manualrun
# (obs-service-go_modules with subdir=proxy).
# TODO: as dependencies land in devel:languages:go, move them to BuildRequires
# and remove from the vendor tarball to consume distro security updates.
Source1:        vendor.tar.zst
BuildRequires:  cmake >= 3.17
BuildRequires:  gcc-c++
BuildRequires:  go >= 1.23
BuildRequires:  libzypp-devel
BuildRequires:  yaml-cpp-devel
BuildRequires:  zstd

%description
mcp-server-zypp is an MCP (Model Context Protocol) server that exposes
openSUSE/SUSE package management operations to LLM agents via libzypp.

It consists of two components:
- mcp-server-zypp: the Go MCP proxy, speaks the MCP protocol over stdio
- zypp-mcp-tool:   the C++ worker, spawned per tool call, links against libzypp

%prep
%autosetup -p1
# Unpack vendor tarball into proxy/ where go.mod lives
tar -x --zstd -f %{SOURCE1} -C proxy/

%build
# ── C++ worker (zypp-mcp-tool) ────────────────────────────────────────────────
# BUILD_GO_PROXY=OFF: cmake only builds the C++ worker; Go is handled below.
%cmake -DBUILD_GO_PROXY=OFF
%cmake_build

# ── Go proxy (mcp-server-zypp) ────────────────────────────────────────────────
# -mod=vendor:    use proxy/vendor/, no network access required.
# -buildmode=pie: position-independent executable, required by openSUSE policy.
# -ldflags "-X":  bake the libexec worker path at link time.
cd %{_builddir}/%{name}-%{version}/proxy
go build \
    -mod=vendor \
    -buildmode=pie \
    -ldflags "-X 'github.com/openSUSE/mcp-server-zypp/internal/config.DefaultWorkerPath=%{_libexecdir}/mcp-server-zypp/zypp-mcp-tool'" \
    -o mcp-server-zypp \
    ./cmd/mcp-server-zypp

%install
# C++ worker — installed by cmake into %%{_libexecdir}/mcp-server-zypp/
%cmake_install

# Go proxy — cmake did not build it, install manually
install -D -m 0755 proxy/mcp-server-zypp %{buildroot}%{_bindir}/mcp-server-zypp

%check
# Integration tests run zypp-mcp-tool against synthetic solver testcases —
# no live system, no root, no network. Mirrors proxy/CMakeLists.txt's
# mcp_worker_tests ctest target, invoked directly here since %%build used
# BUILD_GO_PROXY=OFF (cmake never configured the proxy/ subdirectory that
# target lives in).
MCP_WORKER_BINARY=$(pwd)/build/worker/zypp-mcp-tool \
MCP_TESTDATA_DIR=$(pwd)/tests/testdata \
sh -c 'cd proxy && go test -mod=vendor -v ./tests/...'

%files
%license LICENSE
%doc README.md
%{_bindir}/mcp-server-zypp
%dir %{_libexecdir}/mcp-server-zypp
%{_libexecdir}/mcp-server-zypp/zypp-mcp-tool

%changelog
