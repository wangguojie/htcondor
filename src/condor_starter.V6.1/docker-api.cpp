#include "condor_common.h"

#include "env.h"
#include "my_popen.h"
#include "CondorError.h"
#include "condor_config.h"
#include "condor_classad.h"
#include "condor_daemon_core.h"

#include "docker-api.h"

//
// Because this executes docker run in detached mode, we don't actually
// care if the image is stored locally or not (except to the extent that
// remote image pull violates the principle of least astonishment).
//
int DockerAPI::run(
	const std::string & dockerName,
	const std::string & imageID,
	const std::string & command,
	const ArgList & args,
	const Env & /* env */,
	const std::string & sandboxPath,
	std::string & containerID,
	int & pid,
	CondorError & /* err */ )
{
	//
	// TODO: We currently assume that the system has been configured so that
	// anyone (user) who can run an HTCondor job can also run docker.
	//
	ArgList runArgs;
	std::string docker;
	if( ! param( docker, "DOCKER" ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "DOCKER is undefined.\n" );
		return -1;
	}
	runArgs.AppendArg( docker );
	runArgs.AppendArg( "run" );
	// TODO: Set up the environment (in the container).
	runArgs.AppendArg( "--detach" );
	runArgs.AppendArg( "--name" );
	runArgs.AppendArg( dockerName );
	// TODO: Map the external sanbox to the internal sandbox.
	runArgs.AppendArg( "--workdir" );
	runArgs.AppendArg( sandboxPath );
	runArgs.AppendArg( imageID );
	runArgs.AppendArg( command );
	runArgs.AppendArgsFromArgList( args );

	MyString displayString;
	runArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: %s\n", displayString.c_str() );

	// Read from Docker's combined output and error streams.  On success,
	// it returns a 64-digit hex number.
	FILE * dockerResults = my_popen( runArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to run '%s'.\n", displayString.c_str() );
		return -7;
	}

	char buffer[1024];
	if( NULL == fgets( buffer, 1024, dockerResults ) ) {
		if( errno ) {
			dprintf( D_ALWAYS | D_FAILURE, "Failed to read results from Docker: '%s' (%d)\n", strerror( errno ), errno );
			return -2;
		}
	}

	char rawContainerID[65];
	if( 1 != sscanf( buffer, "%64[A-Fa-f0-9]\n", rawContainerID ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "Docker printed something that doesn't look like a container ID.\n" );
		dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		while( fgets( buffer, 1024, dockerResults ) != NULL ) {
			dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		}
		fclose( dockerResults );
		return -3;
	}

	dprintf( D_FULLDEBUG, "Found raw countainer ID '%s'\n", rawContainerID );
	containerID = rawContainerID;
	fclose( dockerResults );

	// Use containerID to find the PID.
	ArgList inspectArgs;
	inspectArgs.AppendArg( docker );
	inspectArgs.AppendArg( "inspect" );
	inspectArgs.AppendArg( "--format" );
	StringList formatElements(	"ContainerId=\"{{.Id}}\" "
								"Pid={{.State.Pid}} "
								"ExitCode={{.State.ExitCode}} "
								"StartedAt=\"{{.State.StartedAt}}\" "
								"FinishedAt=\"{{.State.FinishedAt}}\" " );
	char * formatArg = formatElements.print_to_delimed_string( "\n" );
	inspectArgs.AppendArg( formatArg );
	free( formatArg );
	inspectArgs.AppendArg( containerID );

	displayString = "";
	inspectArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: %s\n", displayString.c_str() );

	dockerResults = my_popen( inspectArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Unable to run '%s'.\n", displayString.c_str() );
		return -6;
	}

	// If the output isn't exactly formatElements.number() lines long,
	// something has gone wrong and we'll at least be able to print out
	// the error message(s).
	std::string correctOutput[ formatElements.number() ];
	for( int i = 0; i < formatElements.number(); ++i ) {
		if( fgets( buffer, 1024, dockerResults ) != NULL ) {
			correctOutput[i] = buffer;
		}
	}

	int attrCount = 0;
	ClassAd dockerAd;
	for( int i = 0; i < formatElements.number(); ++i ) {
		if( correctOutput[i].empty() || dockerAd.Insert( correctOutput[i].c_str() ) == FALSE ) {
			break;
		}
		++attrCount;
	}

	if( attrCount != formatElements.number() ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to create classad from Docker output (%d).  Printing up to the first %d (nonblank) lines.\n", attrCount, formatElements.number() );
		for( int i = 0; i < formatElements.number() && ! correctOutput[i].empty(); ++i ) {
			dprintf( D_ALWAYS | D_FAILURE, "%s", correctOutput[i].c_str() );
		}
		return -4;
	}
	fclose( dockerResults );

	dprintf( D_FULLDEBUG, "docker inspect printed:\n" );
	for( int i = 0; i < formatElements.number() && ! correctOutput[i].empty(); ++i ) {
		dprintf( D_FULLDEBUG, "\t%s", correctOutput[i].c_str() );
	}

	// If the PID is 0, the job either terminated really quickly or
	// hasn't started yet.
	if( ! dockerAd.LookupInteger( "Pid", pid ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to find pid in Docker output.\n" );
		return -5;
	}

	//
	// It's convenient to have a process which suicides when the container
	// terminates.  If 'docker logs --follow' does, using it allows us to
	// trivially stream I/O, and since it /will/ do 'catch-up', we won't
	// even miss any.  [TODO]
	//
	// Calling daemonCore->Create_Process() handles much of this magic for us.
	//

	ArgList waitArgs;
	waitArgs.AppendArg( docker );
	waitArgs.AppendArg( "wait" );
	waitArgs.AppendArg( containerID );

	FamilyInfo fi;
	fi.max_snapshot_interval = param_integer( "PID_SNAPSHOT_INTERVAL", 15 );

	// We need backticks.
	int childFDs[3];

	// Docker doesn't need stdin.
	childFDs[0] = -1;

	int pipeFDs[2];
	if( ! daemonCore->Create_Pipe( pipeFDs, false, false, true ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "Unable to create pipe to run Docker.\n" );
		return -2;
	}
	// int dockersStdOut = pipeFDs[0];
	childFDs[1] = pipeFDs[1];

	if( ! daemonCore->Create_Pipe( pipeFDs, false, false, true ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "Unable to create pipe to run Docker.\n" );
		return -2;
	}
	// int dockersStdErr = pipeFDs[0];
	childFDs[2] = pipeFDs[1];

	int childPID = daemonCore->Create_Process( docker.c_str(), waitArgs,
		PRIV_UNKNOWN, 1, FALSE, FALSE, NULL, sandboxPath.c_str(),
		& fi, NULL, childFDs );

	if( childPID == FALSE ) {
		dprintf( D_ALWAYS | D_FAILURE, "Create_Process() failed.\n" );
		return -1;
	}
	pid = childPID;

	return 0;
}

int DockerAPI::rm( const std::string & containerID, CondorError & /* err */ ) {
	std::string docker;
	if( ! param( docker, "DOCKER" ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "DOCKER is undefined.\n" );
		return -1;
	}

	ArgList rmArgs;
	rmArgs.AppendArg( docker );
	rmArgs.AppendArg( "rm" );
	rmArgs.AppendArg( containerID.c_str() );

	MyString displayString;
	rmArgs.GetArgsStringForLogging( & displayString );
	dprintf( D_FULLDEBUG, "Attempting to run: %s\n", displayString.c_str() );

	// Read from Docker's combined output and error streams.
	FILE * dockerResults = my_popen( rmArgs, "r", 1 );
	if( dockerResults == NULL ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to run '%s'.\n", displayString.c_str() );
		return -2;
	}

	// On a success, Docker writes the containerID back out.
	char buffer[1024];
	if( NULL == fgets( buffer, 1024, dockerResults ) ) {
		if( errno ) {
			dprintf( D_ALWAYS | D_FAILURE, "Failed to read results from Docker: '%s' (%d)\n", strerror( errno ), errno );
			return -3;
		}
	}

	int length = strlen( buffer );
	if( length < 1 || strncmp( buffer, containerID.c_str(), length - 1 ) != 0 ) {
		dprintf( D_ALWAYS | D_FAILURE, "Docker remove failed, printing first few lines of output.\n" );
		dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		while( NULL != fgets( buffer, 1024, dockerResults ) ) {
			dprintf( D_ALWAYS | D_FAILURE, "%s", buffer );
		}
		return -4;
	}

	return 0;
}
