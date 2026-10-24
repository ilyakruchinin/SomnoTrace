1. First, lets create a secure local user for our SMB share point. This is optional, but protects your user account against leaked credentials. Commonly called a "Service Account", a limited user account has a restricted scope in the actions it can perform. By not making this user an admin, denying local login rights (i.e. cannot log in from the Windows login screen), and limiting the share's user rights to write-only, we limit the attack surface. A complex password generator will make even a brute-force attack much more difficult to accomplish
2.  Logged in as an administrator-enabled account, from the Start Menu type 'Computer Management' and open. From the left pane, click Local Users and Groups ![[LUG_Users.png]]
3. Right click on the Users folder in the right pane and choose 'New User'![[LUG_Users_Context.png|370]]
4. Create the user with a User Name, Full Name, Description and Password. I have used a secure password generator (1Password website) to generate a 20 character password. Save this password in a password vault / manager application. You will need it for the SomnoTrace-side configuration later. Something like 's?-K.XipaqZ3MG8t}h5N' is not easily memorable, but is significantly challenging to brute force. A password strength checker on the example password above says it will take a computer about 3 sextillion years to break the password. ![[New_User_Create.png]]
5. Click create
6. Now, from the Start menu type in 'Edit Group Policy' and open the control panel item associated result
7. In the left pane, expand Computer Configuration -> Security Settings -> Local Policies -> User Rights Assignment ![[GPol_User_Rights_Assign.png]]
8. In the right pane, locate 'Deny log on locally' and double-click to open
9. Click add user or group ![[GPol_Deny_Local_Logon.png]]
10. Type in the name of your service account and click Check Names. The path will expand as shown
    ![[Select_User_Groups.png]]
11. Click OK, then apply, the OK again. Close Computer Management and Local Group Policy Editor
12. Next, using Windows Explorer, create a folder on your computer. In this example, I have C:\Shared\SMB created![[Explorer_SMB.png]]
13. Right-click on SMB, and choose 'Properties' from the popup/context menu ![[Explorer_SMB_Properties.png]]
14.  In the Properties window, select the Sharing tab, and click "Advanced Sharing".
15. In the Advance Sharing window, click "Share this folder" and populate as desired. Keeping simultaneous connections low again reduces the attack plane
Example:
|![[Properties_Adv_Sharing.png]]
16. Click the 'Permissions' button
17. Remove Everyone. Again, we want to force authorizations and limit the opportunity to exploit. To do so, click Everyone, and press the 'Remove' button
![[Folder_Permissions.png]]
18. Click the 'Add...' button and again, enter your SomnoTrace service account user. Ok to add, and then set the following permissions. We will make this a "write only" user next. Click 'Apply' and 'Ok'
    ![[Folder_Permissions_Restricted.png]]
19. Again, click 'Apply' and 'Ok' to close the Advanced Sharing window
20. From the 'Security' tab of the properties window, ![[Folder_Properties_Security.png]]
    Click Edit... on Users. Then 'Add' , enter your SomnoTrace service account name, check names and then Ok
21. You will see the user now in the permissions list. Modify the user to have the rights 'List folder contents' and 'Write'![[Folder_Permissions_Service_Account.png]]
22. 'Apply' then 'Close'
23. Open the SMB folder and create a new folder. Due to Windows "funniness" you cannot write to the 'root' folder readily. Create a folder 'SomnoTrace'
24. Now, open your web browser of choice and go to your Somnotrace homepage, settings tab and scroll to SMB.
25. Turn on the Enable SMB Upload toggle, then populate the fields. Your computer's local IP address is recommended here. As you are running a service, I recommend using DHCP reservation or a Static IP address on your computer. Consult your Router's documentation on a DHCP reservation, or Google for setting a static IP address. Both these are outside the scope of this document. 
    ![[ST_SMB_Share_Name.png]]
26. You'll notice the share name 'Sharing' mirrors the share we created in 'Advanced Sharing' and the SomnoTrace Remote Path is the folder under 'SMB' that was created earlier. The password is that secure password you created for the SomnoTrace service account that you put in your password vault... or go back to Computer Management, Local Users, Users, right click on the SomnoTrace user and click 'set password' to reset it. Now store it securely!
    Click 'Test connection'
27. Success! ![[ST_SMB_Test_OK.png]]
