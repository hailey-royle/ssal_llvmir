#define _XOPEN_SOURCE 600 /* pthread_barrier_t and related */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float f32;
typedef double f64;

#define ANSI_FOREGROUND_RED "\x1b[31m"
#define ANSI_FOREGROUND_DEFAULT "\x1b[39m"

#define MAX_FILE_COUNT         ( 128 )      /* arbitrary: page size / sizeof source_file */
#define MAX_FILE_BUFFER_COUNT  ( 1 << 16 )  /* arbitrary: max is 1 << 32 */
#define MAX_OUTPUT_COUNT       ( 1 << 16 )  /* arbitrary */
#define MAX_SCRATCH_COUNT      ( 1 << 16 )  /* arbitrary */
#define MAX_TOKEN_PAGING_COUNT ( 1 << 16 )  /* max possible */

#define MAX_PROCEDURE_COUNT             MAX_TOKEN_PAGING_COUNT  /* max possible */
#define MAX_STATEMENT_COUNT             MAX_TOKEN_PAGING_COUNT  /* arbitrary: max is MAX_GLOBAL_REGISTER_COUNT + MAX_PAGING_COUNT */
#define MAX_GLOBAL_REGISTER_COUNT       MAX_TOKEN_PAGING_COUNT  /* max possible */
#define MAX_GLOBAL_REGISTER_VALUE_COUNT MAX_TOKEN_PAGING_COUNT  /* arbitrary: max is MAX_GLOBAL_REGISTER_COUNT + MAX_PAGING_COUNT */
#define MAX_TYPE_COUNT                  MAX_TOKEN_PAGING_COUNT  /* max possible */
#define MAX_TYPE_SIGNATURE_COUNT        MAX_TOKEN_PAGING_COUNT  /* arbitrary: max is MAX_GLOBAL_REGISTER_COUNT + MAX_PAGING_COUNT */

#define MAX_IDENTIFIER_COUNT 256

#define TOKEN_ERROR 0
#define TOKEN_PROCEDURE 1
#define TOKEN_ROUTINE 2
#define TOKEN_CALL 3
#define TOKEN_RETURN 4
#define TOKEN_JUMP 5
#define TOKEN_CONDITIONAL 6
#define TOKEN_REGISTER 7
#define TOKEN_ARGUMENT 8
#define TOKEN_USE 9
#define TOKEN_NUMBER 10
#define TOKEN_STRING 11
#define TOKEN_ADDRESS 12
#define TOKEN_TYPE 13
#define TOKEN_ARRAY 14
#define TOKEN_INDEX 15
#define TOKEN_COMPOSITE 16
#define TOKEN_MEMBER 17

#define TOKEN_IDENTIFIER 128
#define TOKEN_SIGNATURE_END 130
#define TOKEN_ASSIGNMENT 132
#define TOKEN_EXPRESSION_START 133
#define TOKEN_EXPRESSION_END 134
#define TOKEN_ARRAY_START 133
#define TOKEN_ARRAY_END 134
#define TOKEN_COMPOSITE_START 135
#define TOKEN_COMPOSITE_END 136

#define TOKEN_EOF 255

#define FLAG_INTRENSIC 0x1

/*
char* token_kind_string[ 256 ] = {
	"error       ",
	"procedure   ",
	"routine     ",
	"call        ",
	"return      ",
	"jump        ",
	"contitional ",
	"register    ",
	"argument    ",
	"use         ",
	"number      ",
	"string      ",
	"address     ",
	"type        ",
	"array       ",
	"index       ",
	"composite   ",
	"member      ",
};
*/

typedef struct {
	i32 raw_index;
	i32 child_index;
	i32 child_count;
	u8 bytes;
	u8 kind;
	u8 flags;
	u8 padding;
} syntax_token;

typedef struct {
	char* name;
	char* raw;
	char* parse;
	i32 name_bytes;
	i32 raw_bytes;
} source_file;

source_file file[ MAX_FILE_COUNT ];
char file_buffer[ MAX_FILE_BUFFER_COUNT ];
i32 file_count = 0;
i32 file_buffer_count = 0;
pthread_mutex_t file_mutex;
_Atomic i32 files_started_parsing = 0;
_Atomic i32 files_finished_parsing = 0;

syntax_token procedure[ MAX_PROCEDURE_COUNT ];
i32 procedure_count = 0;
syntax_token statement[ MAX_STATEMENT_COUNT ];
i32 statement_count = 0;
pthread_mutex_t procedure_mutex;
i32 procedures_started_validating = 0;
i32 procedures_started_outputing = 0;

syntax_token type[ MAX_TYPE_COUNT ];
i32 type_count = 0;
syntax_token type_signature[ MAX_TYPE_SIGNATURE_COUNT ];
i32 type_signature_count = 0;
pthread_mutex_t type_mutex;
i32 types_started_validating = 0;
i32 types_started_outputing = 0;

syntax_token global_register[ MAX_GLOBAL_REGISTER_COUNT ];
i32 global_register_count = 0;
syntax_token global_register_value[ MAX_GLOBAL_REGISTER_VALUE_COUNT ];
i32 global_register_value_count = 0;
pthread_mutex_t global_register_mutex;
i32 global_registers_started_validating = 0;
i32 global_registers_started_outputing = 0;

char output_buffer[ MAX_OUTPUT_COUNT ];
i32 output_buffer_count = 0;
pthread_mutex_t output_mutex;

_Thread_local u8 scratch[ MAX_SCRATCH_COUNT ];
_Thread_local i32 scratch_count = 0;

pthread_barrier_t thread_sync;

void assert_failed( char* file, i32 line, char* expression ){
	fprintf( stderr, "%s%s:%d%s \"%s\"\n", ANSI_FOREGROUND_RED, file, line, ANSI_FOREGROUND_DEFAULT, expression );
	fflush( stderr );
	exit( 1 );
}

#define assert( expression ){\
	if( !( expression )){\
		assert_failed( __FILE__, __LINE__, #expression );\
	}\
}

void error( char* format, ... ){
	fprintf( stderr, "%sError%s ", ANSI_FOREGROUND_RED, ANSI_FOREGROUND_DEFAULT );
	{
		va_list args;
		va_start( args, format );
		vfprintf( stderr, format, args );
		va_end( args );
	}
	fprintf( stderr, "\n" );
	fflush( stderr );
	exit( 1 );
}

void buffer_append( char* dst, i32* dst_count, char* src, i32 src_count ){
	assert( dst != NULL );
	assert( *dst_count >= 0 );
	assert( src != NULL );
	assert( src_count >= 0 );
	memmove( &dst[ *dst_count ], src, src_count );
	*dst_count += src_count;
}

void buffer_insert( char* dst, i32* dst_count, i32 index, char* src, i32 src_count ){
	assert( dst != NULL );
	assert( *dst_count >= 0 );
	assert( index >= 0 );
	assert( src != NULL );
	assert( src_count >= 0 );
	memmove( &dst[ index + src_count ], &dst[ index ], *dst_count - index );
	memmove( &dst[ index ], src, src_count );
	*dst_count += src_count;
}

void buffer_delete( char* dst, i32* dst_count, i32 index, i32 count ){
	assert( dst != NULL );
	assert( *dst_count >= 0 );
	assert( index >= 0 );
	assert( count >= 0 );
	assert( index + count <= *dst_count );
	memmove( &dst[ index ], &dst[ index + count ], *dst_count - index - count );
	*dst_count -= count;
}

void file_add( char* name, i32 name_bytes ){
	assert( name != NULL );
	assert( name_bytes > 0 );
	assert( name_bytes <= 4096 );
	assert( MAX_FILE_COUNT > file_count + 1 );
	char name_null_terminated[ MAX_IDENTIFIER_COUNT ] = { 0 };
	memcpy( name_null_terminated, name, name_bytes );
	name_null_terminated[ name_bytes ] = '\0';
	FILE* fp = fopen( name_null_terminated, "r" );
	if( fp == NULL ){
		printf( "%sError%s Failed to open file '%s'.\n", ANSI_FOREGROUND_RED, ANSI_FOREGROUND_DEFAULT, name_null_terminated );
		exit( 1 );
	}
	i32 error = fseek( fp, 0, SEEK_END );
	assert( !error );
	i64 file_length = ftell( fp );
	assert( file_length >= 0 );
	rewind( fp );
/* critical section */
	error = pthread_mutex_lock( &file_mutex );
	assert( !error );
	assert( MAX_FILE_BUFFER_COUNT > file_buffer_count + file_length );
	i64 fread_result = fread( &file_buffer[ file_buffer_count ], 1, file_length, fp );
	assert( fread_result == file_length );
	file[ file_count ].name = name;
	file[ file_count ].name_bytes = name_bytes;
	file[ file_count ].raw_bytes = file_length;
	file[ file_count ].raw = &file_buffer[ file_buffer_count ];
	file[ file_count ].parse = file[ file_count ].raw;
	file_buffer_count += file_length;
	file_count += 1;
	error = pthread_mutex_unlock( &file_mutex );
/* end critical section */
	assert( !error );
	fclose( fp );
}

_Atomic i32 found_error = 0;

void compiler_error( syntax_token token, char* message ){
/* absolute hack to prevent multiple threads trying to report errors at the same time. */
	if( found_error == 0 ) found_error = 1; else sleep( 1 );
	printf( "Error %s token{ .raw_index = %d .child_index = %d .child_count = %d .bytes = %d .kind = %d }\n%s\n",
	        message, token.raw_index, token.child_index, token.child_count, token.bytes, token.kind, &file_buffer[ token.raw_index ]
	);
	exit( 1 );
}

syntax_token next_token( source_file* file ){
	assert( file != NULL );
	syntax_token token = { 0 };
	char* file_max = file->raw + file->raw_bytes;
	while( 1 ){
		while( file->parse < file_max && ( *file->parse == ' ' || *file->parse == '\n' || *file->parse == '\r' || *file->parse == '\t' )){
			file->parse++;
		}
		if( *file->parse == '{' ){
			i32 depth = 1;
			file->parse++;
			while( file->parse < file_max && depth > 0 ){
				if( *file->parse == '{' ){
					depth += 1;
				} else if( *file->parse == '}' ){
					depth -= 1;
				}
				file->parse++;
			}
			if( file->parse == file_max && depth > 0 ){
				token.raw_index = file->parse - file->raw;
				token.bytes = 1;
				compiler_error( token, "Unclosed comment." );
			}
		} else if( *file->parse == '}' ){
			token.raw_index = file->parse - file->raw;
			token.bytes = 1;
			compiler_error( token, "Unopened comment." );
		} else {
			break;
		}
	}
	assert( file->parse <= file_max )
	if( file->parse == file_max ){
		token.kind = TOKEN_EOF;
		return token;
	}
	token.raw_index = file->parse - file_buffer;
	if( file->parse < file_max && (( *file->parse >= 'a' && *file->parse <= 'z' ) || ( *file->parse >= 'A' && *file->parse <= 'Z' ) || *file->parse == '_' )){
		do {
			token.bytes++;
			file->parse++;
		} while( file->parse < file_max && (( *file->parse >= 'a' && *file->parse <= 'z' ) || ( *file->parse >= 'A' && *file->parse <= 'Z' ) || ( *file->parse >= '0' && *file->parse <= '9' ) || *file->parse == '_' ));
		if( token.bytes == 9 && memcmp( &file_buffer[ token.raw_index ], "procedure", 9 ) == 0 ){
			token.kind = TOKEN_PROCEDURE;
		} else if( token.bytes == 7 && strncmp( &file_buffer[ token.raw_index ], "routine", 7 ) == 0 ){
			token.kind = TOKEN_ROUTINE;
		} else if( token.bytes == 6 && strncmp( &file_buffer[ token.raw_index ], "return", 6 ) == 0 ){
			token.kind = TOKEN_RETURN;
		} else {
			token.kind = TOKEN_IDENTIFIER;
		}
	} else if( file->parse < file_max && *file->parse >= '0' && *file->parse <= '9' ){
		token.kind = TOKEN_NUMBER;
		do {
			token.bytes++;
			file->parse++;
		} while( file->parse < file_max && (( *file->parse >= '0' && *file->parse <= '9' ) || *file->parse == '_' ));
	} else if( file->parse < file_max && *file->parse == '"' ){
		token.kind = TOKEN_STRING;
		do {
			token.bytes++;
			file->parse++;
		} while( file->parse < file_max && *file->parse != '"' );
		if( file->parse == file_max ){
			compiler_error( token, "Unclosed string" );
		}
		token.bytes++;
		file->parse++;
	} else if( *file->parse == '!' ){
		token.bytes = 1;
		token.kind = TOKEN_JUMP;
		file->parse++;
	} else if( *file->parse == '?' ){
		token.bytes = 1;
		token.kind = TOKEN_CONDITIONAL;
		file->parse++;
	} else if( *file->parse == '^' ){
		token.bytes = 1;
		token.kind = TOKEN_ADDRESS;
		file->parse++;
	} else if( *file->parse == ':' ){
		token.bytes = 1;
		token.kind = TOKEN_SIGNATURE_END;
		file->parse++;
	} else if( *file->parse == '=' ){
		token.bytes = 1;
		token.kind = TOKEN_ASSIGNMENT;
		file->parse++;
	} else if( *file->parse == '(' ){
		token.bytes = 1;
		token.kind = TOKEN_EXPRESSION_START;
		file->parse++;
	} else if( *file->parse == ')' ){
		token.bytes = 1;
		token.kind = TOKEN_EXPRESSION_END;
		file->parse++;
	} else if( *file->parse == '[' ){
		token.bytes = 1;
		token.kind = TOKEN_ARRAY_START;
		file->parse++;
	} else if( *file->parse == ']' ){
		token.bytes = 1;
		token.kind = TOKEN_ARRAY_END;
		file->parse++;
	} else if( *file->parse == '<' ){
		token.bytes = 1;
		token.kind = TOKEN_COMPOSITE_START;
		file->parse++;
	} else if( *file->parse == '>' ){
		token.bytes = 1;
		token.kind = TOKEN_COMPOSITE_END;
		file->parse++;
	} else {
		token.bytes = 1;
		compiler_error( token, "Invalid chracter" );
	}
	assert( token.bytes > 0 );
	assert( token.kind > 0 );
	return token;
}

syntax_token* push_token( syntax_token token ){
	assert( token.bytes > 0 );
	assert(( token.kind & 0x80 ) == 0 );
	assert( MAX_SCRATCH_COUNT > ( scratch_count + 1 ) * sizeof( syntax_token ));
	((syntax_token*)scratch)[ scratch_count ] = token;
	scratch_count += 1;
	return &((syntax_token*)scratch)[ scratch_count - 1 ];
}

void parse_expression( source_file* file, syntax_token token ){
	assert( file != NULL );
	assert( token.bytes > 0 );
	assert( token.kind != TOKEN_ERROR );
	if( token.kind == TOKEN_NUMBER || token.kind == TOKEN_STRING ){
		push_token( token );
	} else if( token.kind == TOKEN_IDENTIFIER ){
		token.kind = TOKEN_USE;
		push_token( token );
	} else if( token.kind == TOKEN_EXPRESSION_START ){
		token = next_token( file );
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Expression must start with procedure / operator / intrensic." );
		}
		token.kind = TOKEN_CALL;
		syntax_token* expression = push_token( token );
		i32 expression_start = scratch_count;
		token = next_token( file );
		do {
			parse_expression( file, token );
			token = next_token( file );
		} while( token.kind != TOKEN_EXPRESSION_END );
		expression->child_count = scratch_count - expression_start;
	} else if( token.kind == TOKEN_ARRAY_START ){
		token = next_token( file );
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Array index must start with name of array." );
		}
		token.kind = TOKEN_ARRAY;
		syntax_token* expression = push_token( token );
		i32 expression_start = scratch_count;
		parse_expression( file, next_token( file ));
		expression->child_count = scratch_count - expression_start;
		token = next_token( file );
		if( token.kind != TOKEN_ARRAY_END ){
			compiler_error( token, "Expected array index end ']'." );
		}
	} else if( token.kind == TOKEN_COMPOSITE_START ){
		token = next_token( file );
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Composite member must start with name of composite." );
		}
		token.kind = TOKEN_COMPOSITE;
		syntax_token* expression = push_token( token );
		i32 expression_start = scratch_count;
		token = next_token( file );
		if( token.kind == TOKEN_ARRAY_START || token.kind == TOKEN_COMPOSITE_START ){
			parse_expression( file, token );
		} else if( token.kind == TOKEN_IDENTIFIER ){
			token.kind = TOKEN_MEMBER;
			push_token( token );
		} else {
			compiler_error( token, "Composite member can only be a member, sub member, or array index." );
		}
		expression->child_count = scratch_count - expression_start;
		token = next_token( file );
		if( token.kind != TOKEN_COMPOSITE_END ){
			compiler_error( token, "Expected composite member end '>'." );
		}
	} else {
		compiler_error( token, "Invalid expression value" );
	}
}

void parse_type( source_file* file, syntax_token token ){
	assert( file != NULL );
	assert( token.bytes > 0 );
	assert( token.kind != TOKEN_ERROR );
	while( token.kind != TOKEN_IDENTIFIER ){
		if( token.kind == TOKEN_ADDRESS ){
			push_token( token );
			token = next_token( file );
		} else if( token.kind == TOKEN_ARRAY_START ){
			token.kind = TOKEN_ARRAY;
			push_token( token );
			token = next_token( file );
			if( token.kind != TOKEN_ARRAY_END ){
				parse_expression( file, token );
				token = next_token( file );
				if( token.kind != TOKEN_ARRAY_END ){
					compiler_error( token, "Expected array end ']'." );
				}
			}
			token = next_token( file );
		} else {
			compiler_error( token, "Expected type." );
		}
	}
	token.kind = TOKEN_TYPE;
	push_token( token );
}

void parse_arguments( source_file* file ){
	assert( file != NULL );
	syntax_token token = next_token( file );
	while( token.kind != TOKEN_SIGNATURE_END ){
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Expected argument name." );
		}
		token.kind = TOKEN_ARGUMENT;
		push_token( token );
		parse_type( file, next_token( file ));
		token = next_token( file );
	}
}

void parse_jump( source_file* file, syntax_token token ){
	assert( file != NULL );
	assert( token.bytes > 0 );
	assert( token.kind == TOKEN_JUMP );
	token = next_token( file );
	if( token.kind == TOKEN_IDENTIFIER ){
		token.kind = TOKEN_JUMP;
		push_token( token );
	} else if( token.kind == TOKEN_EXPRESSION_START ){
		token = next_token( file );
		if( token.kind == TOKEN_IDENTIFIER ){
			token.kind = TOKEN_JUMP;
		} else if( token.kind == TOKEN_RETURN ){
		} else {
			compiler_error( token, "Expected jump location." );
		}
		syntax_token* statement_token = push_token( token );
		i32 statement_start = scratch_count;
		token = next_token( file );
		do {
			parse_expression( file, token );
			token = next_token( file );
		} while( token.kind != TOKEN_EXPRESSION_END );
		statement_token->child_count = scratch_count - statement_start;
	} else {
		compiler_error( token, "Expected jump location or jump expression." );
	}
}

void parse_file( source_file* file ){
	assert( file != NULL );
	syntax_token token = next_token( file );
	syntax_token type_token = next_token( file );
	while( token.kind != TOKEN_EOF ){
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Expected name of global register / procedure / type." );
		}
		if( type_token.kind == TOKEN_PROCEDURE ){
			token.kind = TOKEN_PROCEDURE;
			assert( scratch_count == 0 );
			push_token( token );
			{ /* return types */
				token = next_token( file );
				while( token.kind != TOKEN_SIGNATURE_END ){
					parse_type( file, token );
					token = next_token( file );
				}
			}
			type_token.kind = TOKEN_ROUTINE;
			syntax_token* routine_token = push_token( type_token );
			i32 routine_start = scratch_count;
			parse_arguments( file );
			while( 1 ){
				token = next_token( file );
				if( token.kind == TOKEN_IDENTIFIER ){
					token.kind = TOKEN_REGISTER;
					syntax_token* statement_token = push_token( token );
					i32 statement_start = scratch_count;
					parse_type( file, next_token( file ));
					token = next_token( file );
					if( token.kind != TOKEN_ASSIGNMENT ){
						compiler_error( token, "Expected register assignment." );
					}
					parse_expression( file, next_token( file ));
					statement_token->child_count = scratch_count - statement_start;
				} else if( token.kind == TOKEN_EXPRESSION_START ){
					parse_expression( file, token );
				} else if( token.kind == TOKEN_JUMP ){
					parse_jump( file, token );
					routine_token->child_count = scratch_count - routine_start;
					goto potential_next_routine;
				} else if( token.kind == TOKEN_CONDITIONAL ){
					syntax_token* statement_token = push_token( token );
					i32 statement_start = scratch_count;
					parse_expression( file, token );
					parse_jump( file, next_token( file ));
					parse_jump( file, next_token( file ));
					statement_token->child_count = scratch_count - statement_start;
					routine_token->child_count = scratch_count - routine_start;
					goto potential_next_routine;
				} else {
					compiler_error( token, "Expected start of statement" );
				}
				continue;
			potential_next_routine:
				{
					token = next_token( file );
					type_token = next_token( file );
					if( type_token.kind != TOKEN_ROUTINE ){
						break;
					}
					token.kind = TOKEN_ROUTINE;
					routine_token = push_token( token );
					routine_start = scratch_count;
					parse_arguments( file );
				}
			}
			assert( scratch_count > 0 );
			assert( ((syntax_token*)scratch)[ 0 ].kind == TOKEN_PROCEDURE );
/* critical section */
			i32 error = pthread_mutex_lock( &procedure_mutex );
			assert( !error );
			assert( MAX_PROCEDURE_COUNT > procedure_count + 1 );
			assert( MAX_STATEMENT_COUNT > statement_count + scratch_count - 1 );
			((syntax_token*)scratch)[ 0 ].child_count = scratch_count - 1;
			((syntax_token*)scratch)[ 0 ].child_index = statement_count;
			procedure[ procedure_count ] = ((syntax_token*)scratch)[ 0 ];
			memcpy( &statement[ statement_count ], &((syntax_token*)scratch)[ 1 ], ( scratch_count - 1 ) * sizeof( syntax_token ));
			procedure_count += 1;
			statement_count += scratch_count - 1;
			error = pthread_mutex_unlock( &procedure_mutex );
/* end critical section */
			assert( !error );
		} else if( type_token.kind == TOKEN_TYPE ){
			token.kind = TOKEN_TYPE;
			push_token( token );
			parse_arguments( file );
			assert( scratch_count > 0 );
			assert( ((syntax_token*)scratch)[ 0 ].kind == TOKEN_TYPE );
/* critical section */
			i32 error = pthread_mutex_lock( &type_mutex );
			assert( !error );
			assert( MAX_TYPE_COUNT > type_count + 1 );
			assert( MAX_TYPE_SIGNATURE_COUNT > type_signature_count + scratch_count - 1 );
			((syntax_token*)scratch)[ 0 ].child_count = scratch_count - 1;
			((syntax_token*)scratch)[ 0 ].child_index = type_signature_count;
			type[ type_count ] = ((syntax_token*)scratch)[ 0 ];
			memcpy( &type_signature[ type_signature_count ], &((syntax_token*)scratch)[ 1 ], ( scratch_count - 1 ) * sizeof( syntax_token ));
			type_count += 1;
			type_signature_count += scratch_count - 1;
			error = pthread_mutex_unlock( &type_mutex );
/* end critical section */
			assert( !error );
		} else if( type_token.kind == TOKEN_IDENTIFIER || type_token.kind == TOKEN_ARRAY_START || type_token.kind == TOKEN_ADDRESS ){
			token.kind = TOKEN_REGISTER;
			push_token( token );
			parse_type( file, type_token );
			token = next_token( file );
			if( token.kind != TOKEN_ASSIGNMENT ){
				compiler_error( token, "Expected register assignment." );
			}
			parse_expression( file, next_token( file ));
			assert( scratch_count > 0 );
			assert( ((syntax_token*)scratch)[ 0 ].kind == TOKEN_REGISTER );
/* critical section */
			i32 error = pthread_mutex_lock( &global_register_mutex );
			assert( !error );
			assert( MAX_GLOBAL_REGISTER_COUNT > global_register_count + 1 );
			assert( MAX_GLOBAL_REGISTER_VALUE_COUNT > global_register_value_count + scratch_count - 1 );
			((syntax_token*)scratch)[ 0 ].child_count = scratch_count - 1;
			((syntax_token*)scratch)[ 0 ].child_index = global_register_value_count;
			global_register[ global_register_count ] = ((syntax_token*)scratch)[ 0 ];
			memcpy( &global_register_value[ global_register_value_count ], &((syntax_token*)scratch)[ 1 ], ( scratch_count - 1 ) * sizeof( syntax_token ));
			global_register_count += 1;
			global_register_value_count += scratch_count - 1;
			error = pthread_mutex_unlock( &global_register_mutex );
/* end critical section */
			assert( !error );
		} else {
			compiler_error( token, "Expected type of global register / procedure / type." );
		}
		scratch_count = 0;
	}
}

void validate_is_type( syntax_token* token ){
	assert( token->kind == TOKEN_TYPE || token->kind == TOKEN_ARRAY || token->kind == TOKEN_ADDRESS );
	while( token->kind == TOKEN_ARRAY || token->kind == TOKEN_ADDRESS ){
		token++;
	}
	syntax_token* i = type;
	while( i < &type[ type_count ]){
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			return;
		}
		i++;
	}
	compiler_error( *token, "Expected type." );
}

void validate_is_not_type( syntax_token* token ){
	syntax_token* i = type;
	while( i < &type[ type_count ]){
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision, is type." );
		}
		i++;
	}
}

void validate_is_not_global_register( syntax_token* token ){
	syntax_token* i = global_register;
	while( i < &global_register[ global_register_count ]){
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision, is global_register." );
		}
		i++;
	}
}

void validate_is_not_procedure( syntax_token* token ){
	syntax_token* i = procedure;
	while( i < &procedure[ procedure_count ]){
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision, is procedure." );
		}
		i++;
	}
}

void validate_is_not_routine( syntax_token procedure, syntax_token* token ){
	assert( procedure.kind == TOKEN_PROCEDURE );
	assert( token > &statement[ procedure.child_index ]);
	assert( token < &statement[ procedure.child_index + procedure.child_count ]);
	syntax_token* i = &statement[ procedure.child_index ];
	while( token->kind == TOKEN_TYPE || token->kind == TOKEN_ARRAY || token->kind == TOKEN_ADDRESS ){
		i++;
	}
	assert( i->kind == TOKEN_ROUTINE );
	while( i < &statement[ procedure.child_index + procedure.child_count ]){
		assert( i->kind == TOKEN_ROUTINE );
		assert( i->child_count > 0 );
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			return;
		}
		i += i->child_count;
	}
	compiler_error( *token, "Name collision with routine." );
}

void validate_exclusive_register( syntax_token procedure, syntax_token* token ){
	assert( procedure.kind == TOKEN_PROCEDURE );
	assert( token > &statement[ procedure.child_index ]);
	assert( token < &statement[ procedure.child_index + procedure.child_count ]);
	validate_is_not_type( token );
	validate_is_not_global_register( token );
	validate_is_not_procedure( token );
	validate_is_not_routine( procedure, token );
	syntax_token* i = &statement[ procedure.child_index ];
	while( token->kind == TOKEN_TYPE || token->kind == TOKEN_ARRAY || token->kind == TOKEN_ADDRESS ){
		i++;
	}
	assert( i->kind == TOKEN_ROUTINE );
	while( i < token ){
		assert( i->kind == TOKEN_ROUTINE || i->kind == TOKEN_REGISTER || i->kind == TOKEN_ARGUMENT || i->kind == TOKEN_CALL || i->kind == TOKEN_JUMP || i->kind == TOKEN_RETURN || i->kind == TOKEN_CONDITIONAL);
		assert( i->child_count > 0 );
		if(( i->kind == TOKEN_ARGUMENT || i->kind == TOKEN_REGISTER ) && i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision with register." );
		}
		i += i->child_count;
	}
}

void validate_expression( syntax_token procedure_token, syntax_token* type_token, syntax_token* token ){
	assert( procedure_token.kind == TOKEN_PROCEDURE );
	assert( token > &statement[ procedure_token.child_index ]);
	assert( token < &statement[ procedure_token.child_index + procedure_token.child_count ]);
	if( type_token != NULL ){
		assert( type_token->kind == TOKEN_TYPE );
	}
	if( token->kind == TOKEN_NUMBER ){
		return;
	} else if( token->kind == TOKEN_STRING ){
		return;
	} else if( token->kind == TOKEN_REGISTER ){
		syntax_token* register_def = &statement[ procedure_token.child_index ];
		while( register_def->kind == TOKEN_TYPE ){
			register_def++;
		}
		assert( register_def->kind == TOKEN_ROUTINE );
		register_def++;
		while( 1 ){
			assert( register_def < &statement[ procedure_token.child_index + procedure_token.child_count ]);
			if( register_def->kind == TOKEN_ROUTINE ){
				assert( 0 );
			} else if( register_def->kind == TOKEN_REGISTER || register_def->kind == TOKEN_ARGUMENT ){
				if( register_def->bytes == token->bytes && memcmp( &file_buffer[ register_def->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
					break;
				}
			} else if( register_def->kind == TOKEN_CALL ){
				assert( 0 );
			} else if( register_def->kind == TOKEN_JUMP || register_def->kind == TOKEN_RETURN ){
				assert( 0 );
			} else if( register_def->kind == TOKEN_CONDITIONAL ){
				assert( 0 );
			} else {
				assert( 0 );
			}
			register_def++;
			assert( register_def <= &statement[ procedure_token.child_index + procedure_token.child_count ]);
			if( register_def == &statement[ procedure_token.child_index + procedure_token.child_count ]){
				compiler_error( *token, "Could not find statement name." );
			}
		}
		
	} else if( token->kind == TOKEN_CALL ){
		syntax_token* call_def = &procedure[ 0 ];
		while( 1 ){
			assert( call_def < &procedure[ procedure_count ]);
			if( call_def ->bytes == token->bytes && memcmp( &file_buffer[ call_def ->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
				break;
			}
			call_def++;
			assert( call_def <= &procedure[ procedure_count ]);
			if( call_def == &procedure[ procedure_count ]){
				compiler_error( *token, "Could not find procedure_token name." );
			}
		}
		if( call_def->flags & FLAG_INTRENSIC ){
			if( call_def->bytes == 3 && memcmp( &file_buffer[ call_def->raw_index ], "add", 3 ) == 0 ){
			} else if( call_def->bytes == 9 && memcmp( &file_buffer[ call_def->raw_index ], "cmp_equal", 9 ) == 0 ){
			} else if( call_def->bytes == 4 && memcmp( &file_buffer[ call_def->raw_index ], "exit", 4 ) == 0 ){
			} else if( call_def->bytes == 3 && memcmp( &file_buffer[ call_def->raw_index ], "log", 3 ) == 0 ){
			} else {
				assert( 0 );
			}
		} else {
/* TODO: argument checking of procedure_tokens */  assert( 0 );
/*
			call_def = &call_def[ call_def->child_index ];
			if( type_token == NULL ){
				if( call_def->kind != TOKEN_ROUTINE ){
					compiler_error( *token, "Expected called procedure_token to have no return value." );
				}
			} else {
				if( call_def->bytes != type_token->bytes || memcmp( &file_buffer[ call_def->raw_index ], &file_buffer[ type_token->bytes ], call_def->bytes ) != 0 ){
					compiler_error( *token, "Procedure type_token and use do not match." );
				}
			}
			call_def++;
			assert( call_def->kind == TOKEN_ROUTINE );
			call_def++;
*/
		}
	} else {
		compiler_error( *token, "Invlaid expression value." );
	}
}

void validate_procedure( i32 procedure_index ){
	assert( procedure_index >= 0 );
	assert( procedure_index < procedure_count );
	{ /* exclusive procedure */
		syntax_token token = procedure[ procedure_index ];
		syntax_token* i = procedure;
		while( i < &procedure[ procedure_index ]){
			if( i->bytes == token.bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token.raw_index ], token.bytes ) == 0 ){
				compiler_error( token, "Duplicate procedure name." );
			}
			i++;
		}
	}
	syntax_token* token = &statement[ procedure[ procedure_index ].child_index ];
	while( token->kind == TOKEN_TYPE || token->kind == TOKEN_ARRAY || token->kind == TOKEN_ADDRESS ){
		validate_is_type( token );
		token += token->child_count + 1;
	}
	while( token < &procedure[ procedure[ procedure_index ].child_count + procedure[ procedure_index ].child_index ]){
		if( token->kind == TOKEN_ROUTINE ){
			validate_is_not_type( token );
			{ /* exclusive routine */
				syntax_token* i = &statement[ procedure[ procedure_index ].child_index ];
				while( token->kind == TOKEN_TYPE || token->kind == TOKEN_ARRAY || token->kind == TOKEN_ADDRESS ){
					i++;
				}
				assert( i->kind == TOKEN_ROUTINE );
				while( i < token ){
					assert( i->kind == TOKEN_ROUTINE );
					assert( i->child_count > 0 );
					if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
						compiler_error( *token, "Routine name collision." );
					}
					i += i->child_count;
				}
			}
			token++;
			while( token->kind == TOKEN_ARGUMENT ){
				validate_exclusive_register( procedure[ procedure_index ], token );
				token++;
				validate_is_type( token );
				token += token->child_count + 1;
			}
		} else if( token->kind == TOKEN_REGISTER ){
			validate_exclusive_register( procedure[ procedure_index ], token );
			token++;
			validate_is_type( token );
			validate_expression( procedure[ procedure_index ], token, token + token->child_count + 1 );
			token += token->child_count + 1;
		} else if( token->kind == TOKEN_CALL ){
			validate_expression( procedure[ procedure_index ], NULL, token );
			token += token->child_count + 1;
		} else if( token->kind == TOKEN_JUMP ){
			assert( 0 );
		} else if( token->kind == TOKEN_CONDITIONAL ){
			assert( 0 );
		} else {
			assert( 0 );
		}
		token += token->child_count;
	}
}

void validate_global_register( i32 global_register_index ){
	assert( global_register_count >= 0 );
	assert( global_register_index < global_register_count );
	assert( global_register_count == 0 );
	assert( 0 );
/*
	assert( global_register_index >= 0 );
	assert( global_register_index < global_register_count );
	syntax_token token = global_register[ global_register_index ];
	syntax_token* i = global_register;
	while( i < &global_register[ global_register_index ]){
		if( i->bytes == token.bytes && memcmp( &file_buffer[ token.raw_index ], &file_buffer[ i->raw_index ], token.bytes ) == 0 ){
			compiler_error( token, "Two global registers have the same name." );
		}
		i += i->child_count + 1;
	}
	validate_is_not_procedure( &global_register[ global_register_index ]);
*/
}

void validate_type( i32 type_index ){
	assert( type_index >= 0 );
	assert( type_index < type_count );
	syntax_token* token = &type[ type_index ];
	assert( token->flags & FLAG_INTRENSIC );
	syntax_token* i = type;
	while( i < &type[ type_index ]){
		assert( i->flags & FLAG_INTRENSIC );
		if( i->bytes == token->bytes && memcmp( &file_buffer[ token->raw_index ], &file_buffer[ i->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Two types have the same name." );
		}
		i += i->child_count + 1;
	}
	validate_is_not_procedure( token );
	validate_is_not_global_register( token );
}

void push_output(){
/* critical section */
	i32 error = pthread_mutex_lock( &output_mutex );
	assert( !error );
	memcpy( &output_buffer[ output_buffer_count ], scratch, scratch_count );
	output_buffer_count += scratch_count;
	error = pthread_mutex_unlock( &output_mutex );
/* end critical section */
	assert( !error );
	scratch_count = 0;
}

void output_llvmir_procedure( i32 procedure_index ){
	assert( procedure_index >= 0 );
	syntax_token* procedure_token = &procedure[ procedure_index ];
	assert( !( procedure_token->flags & FLAG_INTRENSIC ));
	assert( procedure_token->bytes == 5 && memcmp( &file_buffer[ procedure_token->raw_index ], "start", 5 ) == 0 );
	assert( scratch_count == 0 );
	i32 return_type_index = 7;
	buffer_append( (char*) scratch, &scratch_count, "define  @start( ", 16 );
	syntax_token* token = &statement[ procedure_token->child_index ];
	syntax_token* return_type_token = token;
	token++;
	assert( return_type_token->kind == TOKEN_TYPE );
	buffer_insert( (char*) scratch, &scratch_count, return_type_index, &file_buffer[ return_type_token->raw_index ], return_type_token->bytes );
	assert( token->kind == TOKEN_ROUTINE );
	token++;
	while( token->kind == TOKEN_ARGUMENT ){
		assert( token->bytes == 8 && memcmp( &file_buffer[ token->raw_index ], "argument", 8 ) == 0 );
		token++;
		assert( token->kind == TOKEN_ARRAY );
		token++;
		assert( token->kind == TOKEN_ARRAY );
		token++;
		assert( token->kind == TOKEN_TYPE );
		assert( token->bytes == 2 && memcmp( &file_buffer[ token->raw_index ], "i8", 2 ) == 0 );
		buffer_append( (char*) scratch, &scratch_count, "i32 %argument.count, i32 %argument.capacity, ptr %argument.data", 63 ); 
		token++;
	}
	buffer_append( (char*) scratch, &scratch_count, " ){\n", 4 );
	while( token < &statement[ procedure_token->child_index + procedure_token->child_count ]){
		if( token->kind == TOKEN_RETURN ){
			buffer_append( (char*) scratch, &scratch_count, "\tret ", 5 );
			buffer_append( (char*) scratch, &scratch_count, &file_buffer[ return_type_token->raw_index ], return_type_token->bytes );
			token++;
			if( token->kind == TOKEN_NUMBER ){
				buffer_append( (char*) scratch, &scratch_count, " ", 1 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
			} else if( token->kind == TOKEN_USE ){
				buffer_append( (char*) scratch, &scratch_count, " %", 2 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
			} else {
				assert( 0 );
			}
			buffer_append( (char*) scratch, &scratch_count, "\n", 1 );
			token++;
		} else if( token->kind == TOKEN_CALL ){
			buffer_append( (char*) scratch, &scratch_count, "\tcall void @", 12 );
			buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
			token++;
			buffer_append( (char*) scratch, &scratch_count, "( ", 2 );
			if( token->kind == TOKEN_NUMBER ){
				buffer_append( (char*) scratch, &scratch_count, "i32 ", 5 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
			} else {
				assert( 0 );
			}
			buffer_append( (char*) scratch, &scratch_count, " )\n", 3 );
			token++;
		} else if( token->kind == TOKEN_REGISTER ){
			buffer_append( (char*) scratch, &scratch_count, "\t%", 2 );
			buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
			token++;
			token++;
			if( token->kind == TOKEN_NUMBER ){
				buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token[ -1 ].raw_index ], token[ -1 ].bytes );
				buffer_append( (char*) scratch, &scratch_count, " 0, ", 4 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
			} else if( token->kind == TOKEN_USE ){
				buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token[ -1 ].raw_index ], token[ -1 ].bytes );
				buffer_append( (char*) scratch, &scratch_count, " 0, %", 5 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
			} else if( token->kind == TOKEN_CALL ){
				buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
				buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token[ -1 ].raw_index ], token[ -1 ].bytes );
				token++;
				if( token->kind == TOKEN_NUMBER ){
					buffer_append( (char*) scratch, &scratch_count, " ", 1 );
					buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
				} else if( token->kind == TOKEN_USE ){
					buffer_append( (char*) scratch, &scratch_count, " %", 2 );
					buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
				} else {
					assert( 0 );
				}
				token++;
				if( token->kind == TOKEN_NUMBER ){
					buffer_append( (char*) scratch, &scratch_count, ", ", 2 );
					buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
				} else if( token->kind == TOKEN_USE ){
					buffer_append( (char*) scratch, &scratch_count, ", %", 3 );
					buffer_append( (char*) scratch, &scratch_count, &file_buffer[ token->raw_index ], token->bytes );
				} else {
					assert( 0 );
				}
			} else {
				assert( 0 );
			}
			buffer_append( (char*) scratch, &scratch_count, "\n", 1 );
			token++;
		} else {
			assert( 0 );
		}
	}
	buffer_append( (char*) scratch, &scratch_count, "}\n", 2 );
	push_output();
}

/*
void output_llvmir_procedure( syntax_token* procedure ){
	assert( procedure != NULL );
	assert( procedure->bytes > 0 );
	assert( procedure->child_count > 0 );
	assert( procedure->kind == TOKEN_PROCEDURE );
	while( 1 ){
		assert( procedure->bytes == 5 && strncmp( procedure->raw, "start", 5 ) == 0 );
		i32 return_type_index = scratch_count + 7;
		buffer_append( (char*) scratch, &scratch_count, "define  @start( ", 16 );
		syntax_token* return_type_token = procedure + 1;
		assert( return_type_token->kind == TOKEN_TYPE );
		buffer_insert( (char*) scratch, &scratch_count, return_type_index, return_type_token->raw, return_type_token->bytes );
		syntax_token* token = return_type_token + 1;
		assert( token->kind == TOKEN_ROUTINE );
		token++;
		if( token->kind == TOKEN_ARGUMENT ){
			token++;
			assert( token->kind == TOKEN_TYPE );
			buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
			buffer_append( (char*) scratch, &scratch_count, " %", 2 );
			buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
			token++;
		}
		buffer_append( (char*) scratch, &scratch_count, " ){\n", 4 );
		while( 1 ){
			if( token->kind == TOKEN_REGISTER ){
				buffer_append( (char*) scratch, &scratch_count, "\t%", 2 );
				buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				token = token + 1;
				assert( token->kind == TOKEN_TYPE );
				token = token + 1;
				if( token->kind == TOKEN_NUMBER ){
					buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
					buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
					buffer_append( (char*) scratch, &scratch_count, " 0, ", 4 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else if( token->kind == TOKEN_REGISTER ){
					buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
					buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
					buffer_append( (char*) scratch, &scratch_count, " 0, %", 5 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else if( token->kind == TOKEN_ADD ){
					buffer_append( (char*) scratch, &scratch_count, " = add ", 7 );
					buffer_append( (char*) scratch, &scratch_count, token[ -1 ].raw, token[ -1 ].bytes );
					token = token + 1;
					if( token->kind == TOKEN_NUMBER ){
						buffer_append( (char*) scratch, &scratch_count, " ", 1 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else if( token->kind == TOKEN_REGISTER ){
						buffer_append( (char*) scratch, &scratch_count, " %", 2 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else {
						assert( 0 );
					}
					token = token + 1;
					if( token->kind == TOKEN_NUMBER ){
						buffer_append( (char*) scratch, &scratch_count, ", ", 2 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else if( token->kind == TOKEN_REGISTER ){
						buffer_append( (char*) scratch, &scratch_count, ", %", 3 );
						buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
					} else {
						assert( 0 );
					}
				} else {
					assert( 0 );
				}
				buffer_append( (char*) scratch, &scratch_count, "\n", 1 );
			} else if( token->kind == TOKEN_RETURN ){
				buffer_append( (char*) scratch, &scratch_count, "\tret ", 5 );
				buffer_append( (char*) scratch, &scratch_count, return_type_token->raw, return_type_token->bytes );
				token = token + 1;
				if( token->kind == TOKEN_NUMBER ){
					buffer_append( (char*) scratch, &scratch_count, " ", 1 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else if( token->kind == TOKEN_REGISTER ){
					buffer_append( (char*) scratch, &scratch_count, " %", 2 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else {
					assert( 0 );
				}
				buffer_append( (char*) scratch, &scratch_count, "\n}\n", 3 );
				break;
			} else if( token->kind == TOKEN_CALL ){
				assert( token->bytes == 4 && strncmp( token->raw, "exit", token->bytes ) == 0 );
				buffer_append( (char*) scratch, &scratch_count, "\tcall void @exit( i32", 21 );
				token++;
				if( token->kind == TOKEN_NUMBER ){
					buffer_append( (char*) scratch, &scratch_count, " ", 1 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else if( token->kind == TOKEN_REGISTER ){
					buffer_append( (char*) scratch, &scratch_count, " %", 2 );
					buffer_append( (char*) scratch, &scratch_count, token->raw, token->bytes );
				} else {
					assert( 0 );
				}
				buffer_append( (char*) scratch, &scratch_count, " )\n", 3 );
			} else {
				assert( 0 );
			}
			token++;
		}
		push_output();
		break;
	}
	procedures_finished_outputing++;
}
*/

void output_procedure( i32 index ){
	assert( index >= 0 );
	syntax_token* procedure_token = &procedure[ index ];
	if( procedure_token->flags & FLAG_INTRENSIC ){
		return;
	}
	output_llvmir_procedure( index );
}

void output_global_register( i32 index ){
	assert( index >= 0 );
	assert( 0 );
	return;
}

void output_type( i32 index ){
	assert( index >= 0 );
	assert( type[ index ].flags & FLAG_INTRENSIC );
	return;
}

void* thread_main( void* argt ){
	i64 thread_index = (i64) argt;
	while( 1 ){
/*
  If a file is ready to be parsed, go parse it.
  If all files have been parsed, break out of the loop.
  Else unlock and try again.
*/
/* critical section */
		i32 error = pthread_mutex_lock( &file_mutex );
		assert( !error );
		assert( file_count > 0 );
		assert( files_started_parsing >= 0 );
		assert( files_finished_parsing >= 0 );
		assert( file_count >= files_started_parsing );
		assert( file_count >= files_finished_parsing );
		assert( files_started_parsing >= files_finished_parsing );
		if( file_count == files_finished_parsing ){
			error = pthread_mutex_unlock( &file_mutex );
			assert( !error );
			break;
		}
		if( files_started_parsing < file_count ){
			i16 index = files_started_parsing;
			files_started_parsing++;
			error = pthread_mutex_unlock( &file_mutex );
			assert( !error );
			parse_file( &file[ index ]);
			files_finished_parsing++;
		} else {
			error = pthread_mutex_unlock( &file_mutex );
			assert( !error );
		}
/* end critical section */
	}
	{
		while( 1 ){
			i32 index = __atomic_fetch_add( &types_started_validating, 1, __ATOMIC_ACQ_REL );
			if( index >= type_count ){
				break;
			}
			validate_type( index );
		}
		while( 1 ){
			i32 index = __atomic_fetch_add( &global_registers_started_validating, 1, __ATOMIC_ACQ_REL );
			if( index >= global_register_count ){
				break;
			}
			validate_global_register( index );
		}
		while( 1 ){
			i32 index = __atomic_fetch_add( &procedures_started_validating, 1, __ATOMIC_ACQ_REL );
			if( index >= procedure_count ){
				break;
			}
			validate_procedure( index );
		}
	}
#ifndef SINGLE_THREAD
	pthread_barrier_wait( &thread_sync );
#endif
	{
		while( 1 ){
			i32 index = __atomic_fetch_add( &types_started_outputing, 1, __ATOMIC_ACQ_REL );
			if( index >= type_count ){
				break;
			}
			output_type( index );
		}
		while( 1 ){
			i32 index = __atomic_fetch_add( &global_registers_started_outputing, 1, __ATOMIC_ACQ_REL );
			if( index >= global_register_count ){
				break;
			}
			output_global_register( index );
		}
		while( 1 ){
			i32 index = __atomic_fetch_add( &procedures_started_outputing, 1, __ATOMIC_ACQ_REL );
			if( index >= procedure_count ){
				break;
			}
			output_procedure( index );
		}
	}
	if( thread_index == 0 ){
		char* target = "target triple = \"x86_64-unknown-linux-gnu\"\n\n"
		               "declare void @exit( i32 )\n\n";
		buffer_insert( output_buffer, &output_buffer_count, 0, target, strlen( target ));
		char output_name[ 260 ] = { 0 };
		memcpy( output_name, file[ 0 ].name, file[ 0 ].name_bytes );
		memcpy( &output_name[ file[ 0 ].name_bytes - 5 ], ".ll\0", 4 );
		FILE* fp = fopen( output_name, "w" );
		i32 write_bytes = fwrite( output_buffer, 1, output_buffer_count, fp );
		assert( write_bytes == output_buffer_count );
		fclose( fp );
	}
	return NULL;
}

i32 main( i32 argc, char* argv[] ){
	if( argc != 2 ){
		error( "Usage: ssal_llvmir <filename>" );
	}
	pthread_mutex_init( &file_mutex, NULL );
	pthread_mutex_init( &procedure_mutex, NULL );
	pthread_mutex_init( &type_mutex, NULL );
	pthread_mutex_init( &global_register_mutex, NULL );
	pthread_mutex_init( &output_mutex, NULL );
	{
		type[ 0 ].raw_index = file_buffer_count;
		type[ 0 ].bytes = 3;
		buffer_append( file_buffer, &file_buffer_count, "i32", type[ 0 ].bytes );
		type[ 0 ].child_index = -1;
		type[ 0 ].child_count = 0;
		type[ 0 ].kind = TOKEN_TYPE;
		type[ 0 ].flags = FLAG_INTRENSIC;
		type[ 1 ].raw_index = file_buffer_count;
		type[ 1 ].bytes = 2;
		buffer_append( file_buffer, &file_buffer_count, "i8", type[ 1 ].bytes );
		type[ 1 ].child_index = -1;
		type[ 1 ].child_count = 0;
		type[ 1 ].kind = TOKEN_TYPE;
		type[ 1 ].flags = FLAG_INTRENSIC;
		type_count = 2;
		procedure[ 0 ].raw_index = file_buffer_count;
		procedure[ 0 ].bytes = 3;
		buffer_append( file_buffer, &file_buffer_count, "add", procedure[ 0 ].bytes );
		procedure[ 0 ].child_index = -1;
		procedure[ 0 ].child_count = 0;
		procedure[ 0 ].kind = TOKEN_PROCEDURE;
		procedure[ 0 ].flags = FLAG_INTRENSIC;
		procedure[ 1 ].raw_index = file_buffer_count;
		procedure[ 1 ].bytes = 9;
		buffer_append( file_buffer, &file_buffer_count, "cmp_equal", procedure[ 1 ].bytes );
		procedure[ 1 ].child_index = -1;
		procedure[ 1 ].child_count = 0;
		procedure[ 1 ].kind = TOKEN_PROCEDURE;
		procedure[ 1 ].flags = FLAG_INTRENSIC;
		procedure[ 2 ].raw_index = file_buffer_count;
		procedure[ 2 ].bytes = 4;
		buffer_append( file_buffer, &file_buffer_count, "exit", procedure[ 2 ].bytes );
		procedure[ 2 ].child_index = -1;
		procedure[ 2 ].child_count = 0;
		procedure[ 2 ].kind = TOKEN_PROCEDURE;
		procedure[ 2 ].flags = FLAG_INTRENSIC;
		procedure[ 3 ].raw_index = file_buffer_count;
		procedure[ 3 ].bytes = 3;
		buffer_append( file_buffer, &file_buffer_count, "log", procedure[ 3 ].bytes );
		procedure[ 3 ].child_index = -1;
		procedure[ 3 ].child_count = 0;
		procedure[ 3 ].kind = TOKEN_PROCEDURE;
		procedure[ 3 ].flags = FLAG_INTRENSIC;
		procedure_count = 4;
	}
	i32 name_bytes = strlen( argv[ 1 ]);
	file_add( argv[ 1 ], name_bytes );
#ifdef SINGLE_THREAD
	thread_main( (void*) ( 0 ));
#else /* default: use multiple threads */
	i64 cpu_count = sysconf( _SC_NPROCESSORS_ONLN );
	assert( cpu_count > 0 );
	pthread_barrier_init( &thread_sync, NULL, cpu_count );
	pthread_t thread_array[ cpu_count - 1 ];
	i64 i = 0;
	while( i < cpu_count - 1 ){
		i32 error = pthread_create( &thread_array[ i ], NULL, thread_main, (void*) i );
		assert( !error );
		i += 1;
	}
	thread_main( (void*) ( cpu_count - 1 ));
	i = 0;
	while( i < cpu_count - 1 ){
		pthread_join( thread_array[ i ], NULL );
		i += 1;
	}
#endif /* threading */
	return 0;
}
