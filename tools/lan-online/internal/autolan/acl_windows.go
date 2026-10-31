//go:build windows

package autolan

import (
	"errors"
	"fmt"
	"sync"
	"syscall"
	"unsafe"
)

// Access-control helpers (advapi32 through the standard syscall package; no extra module).
// "Private" means: owned by this account, the token's default owner, or Administrators,
// with a DACL whose entries all grant access only
// to this account or to SYSTEM. Directories are created with that DACL protected against
// inheritance, so socket files and records inside inherit exactly it.

var (
	modadvapi32                      = syscall.NewLazyDLL("advapi32.dll")
	procGetNamedSecurityInfoW        = modadvapi32.NewProc("GetNamedSecurityInfoW")
	procGetSecurityDescriptorControl = modadvapi32.NewProc("GetSecurityDescriptorControl")
	procGetAclInformation            = modadvapi32.NewProc("GetAclInformation")
	procGetAce                       = modadvapi32.NewProc("GetAce")
	procConvertStringSDToSD          = modadvapi32.NewProc("ConvertStringSecurityDescriptorToSecurityDescriptorW")
)

const (
	seFileObject          = 1
	ownerSecurityInfo     = 0x1
	daclSecurityInfo      = 0x4
	seDaclPresent         = 0x4
	aclSizeInformationCls = 2
	accessAllowedAceType  = 0
	inheritOnlyAce        = 0x8
	tokenOwnerClass       = 4
	sddlRevision1         = 1
	systemSID             = "S-1-5-18"
	administratorsSID     = "S-1-5-32-544"
)

var identityCache struct {
	once        sync.Once
	user, owner string
	err         error
}

// currentIdentity returns the process token's user SID and default-owner SID as strings.
func currentIdentity() (user, owner string, err error) {
	identityCache.once.Do(func() {
		t, e := syscall.OpenCurrentProcessToken()
		if e != nil {
			identityCache.err = e
			return
		}
		defer t.Close()
		u, e := t.GetTokenUser()
		if e != nil {
			identityCache.err = e
			return
		}
		if identityCache.user, e = u.User.Sid.String(); e != nil {
			identityCache.err = e
			return
		}
		var n uint32
		buf := make([]byte, 64)
		for {
			e = syscall.GetTokenInformation(t, tokenOwnerClass, &buf[0], uint32(len(buf)), &n)
			if e == nil {
				break
			}
			if !errors.Is(e, syscall.ERROR_INSUFFICIENT_BUFFER) || int(n) <= len(buf) {
				identityCache.err = e
				return
			}
			buf = make([]byte, n)
		}
		// TOKEN_OWNER holds one pointer to the SID.
		sid := *(**syscall.SID)(unsafe.Pointer(&buf[0]))
		identityCache.owner, identityCache.err = sid.String()
	})
	return identityCache.user, identityCache.owner, identityCache.err
}

type aceInfo struct {
	typ, flags byte
	mask       uint32
	sid        string
}

type securityInfo struct {
	owner       string
	daclPresent bool
	aces        []aceInfo
}

func readSecurity(path string) (*securityInfo, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return nil, err
	}
	var owner *syscall.SID
	var dacl, sd *byte
	r, _, _ := procGetNamedSecurityInfoW.Call(uintptr(unsafe.Pointer(p)), seFileObject,
		ownerSecurityInfo|daclSecurityInfo, uintptr(unsafe.Pointer(&owner)), 0,
		uintptr(unsafe.Pointer(&dacl)), 0, uintptr(unsafe.Pointer(&sd)))
	if r != 0 {
		return nil, fmt.Errorf("GetNamedSecurityInfo: error %d", r)
	}
	defer syscall.LocalFree(syscall.Handle(uintptr(unsafe.Pointer(sd))))
	info := &securityInfo{}
	if owner != nil {
		if info.owner, err = owner.String(); err != nil {
			return nil, err
		}
	}
	var control uint16
	var revision uint32
	if r, _, _ := procGetSecurityDescriptorControl.Call(uintptr(unsafe.Pointer(sd)),
		uintptr(unsafe.Pointer(&control)), uintptr(unsafe.Pointer(&revision))); r == 0 {
		return nil, errors.New("GetSecurityDescriptorControl failed")
	}
	info.daclPresent = control&seDaclPresent != 0 && dacl != nil
	if !info.daclPresent {
		return info, nil
	}
	var size struct{ aceCount, bytesInUse, bytesFree uint32 }
	if r, _, _ := procGetAclInformation.Call(uintptr(unsafe.Pointer(dacl)), uintptr(unsafe.Pointer(&size)),
		unsafe.Sizeof(size), aclSizeInformationCls); r == 0 {
		return nil, errors.New("GetAclInformation failed")
	}
	for i := uint32(0); i < size.aceCount; i++ {
		var ace *byte
		if r, _, _ := procGetAce.Call(uintptr(unsafe.Pointer(dacl)), uintptr(i), uintptr(unsafe.Pointer(&ace))); r == 0 {
			return nil, errors.New("GetAce failed")
		}
		hdr := unsafe.Pointer(ace)
		entry := aceInfo{typ: *(*byte)(hdr), flags: *(*byte)(unsafe.Add(hdr, 1)), mask: *(*uint32)(unsafe.Add(hdr, 4))}
		if entry.sid, err = (*syscall.SID)(unsafe.Add(hdr, 8)).String(); err != nil {
			return nil, err
		}
		info.aces = append(info.aces, entry)
	}
	return info, nil
}

// checkPrivateSecurity enforces the private-directory model described at the top of this file.
func checkPrivateSecurity(path string) error {
	info, err := readSecurity(path)
	if err != nil {
		return err
	}
	user, tokenOwner, err := currentIdentity()
	if err != nil {
		return err
	}
	return validatePrivateSecurity(info, user, tokenOwner)
}

func validatePrivateSecurity(info *securityInfo, user, tokenOwner string) error {
	// A directory created by an elevated process may be owned by Administrators even
	// when the same account later runs without elevation. The DACL must still grant
	// access only to this exact account and SYSTEM; a group owner alone is insufficient.
	if info.owner != user && info.owner != tokenOwner && info.owner != administratorsSID {
		return errors.New("owned by another account")
	}
	if !info.daclPresent || len(info.aces) == 0 {
		return errors.New("has no access-control list")
	}
	for _, ace := range info.aces {
		if ace.typ != accessAllowedAceType || ace.flags&inheritOnlyAce != 0 {
			return fmt.Errorf("unexpected access-control entry (type %d, flags %#x)", ace.typ, ace.flags)
		}
		if ace.sid != user && ace.sid != systemSID {
			return errors.New("grants access to another account")
		}
	}
	return nil
}

// createPrivateDirectory creates dir with a protected, inheritable DACL for this account and
// SYSTEM. An existing directory is left as it is; callers verify it with checkPrivateDir.
func createPrivateDirectory(dir string) error {
	user, _, err := currentIdentity()
	if err != nil {
		return err
	}
	sddl, err := syscall.UTF16PtrFromString("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + user + ")")
	if err != nil {
		return err
	}
	var sd *byte
	var size uint32
	if r, _, e := procConvertStringSDToSD.Call(uintptr(unsafe.Pointer(sddl)), sddlRevision1,
		uintptr(unsafe.Pointer(&sd)), uintptr(unsafe.Pointer(&size))); r == 0 {
		return fmt.Errorf("security descriptor: %v", e)
	}
	defer syscall.LocalFree(syscall.Handle(uintptr(unsafe.Pointer(sd))))
	sa := syscall.SecurityAttributes{
		Length:             uint32(unsafe.Sizeof(syscall.SecurityAttributes{})),
		SecurityDescriptor: uintptr(unsafe.Pointer(sd)),
	}
	p, err := syscall.UTF16PtrFromString(dir)
	if err != nil {
		return err
	}
	if err := syscall.CreateDirectory(p, &sa); err != nil && !errors.Is(err, syscall.ERROR_ALREADY_EXISTS) {
		return err
	}
	return nil
}
