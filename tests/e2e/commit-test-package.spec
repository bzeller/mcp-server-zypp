# Shared dummy package for e2e scenarios needing a real installable RPM.
# Driven entirely by --define; see e2e_common.py: build_test_rpm().
#   pkg_name, pkg_version  required
#   pkg_requires           adds Requires: (point at a nonexistent capability
#                          to force a solver failure)
#   fail_post              %post exits 1 -> rpm reports
#                          "warning: %post(...) scriptlet failed, exit status 1"
#                          WITHOUT failing the transaction
Name:      %{pkg_name}
Version:   %{pkg_version}
Release:   1
Summary:   Dummy package for mcp-server-zypp e2e tests
License:   MIT
BuildArch: noarch
%{?pkg_requires:Requires:  %{pkg_requires}}

%description
Empty test package, no files.

%if 0%{?fail_post}
%post
echo "simulated %%post failure"
exit 1
%endif

%files

%changelog
