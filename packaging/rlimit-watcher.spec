Name:       rlimit-watcher
Version:    0.0.1
Release:    0
License:    Apache-2.0
Source0:    %{name}-%{version}.tar.xz
Source1001: %{name}.manifest
Summary:    rlimit-watcher
Group:      System/Monitoring

%description
example of rlimit-events usage


%prep
%setup -q
cp %{SOURCE1001} .

%build
gcc -o rlimit-watcher rlimit-watcher.c

%install
mkdir -p %{buildroot}/usr/bin/
cp rlimit-watcher %{buildroot}/usr/bin/

%files
/usr/bin/rlimit-watcher
