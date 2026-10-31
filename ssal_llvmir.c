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

#define NUMBER_ERROR 0
#define NUMBER_INTEGER 1
#define NUMBER_NEGITIVE_INTEGER 2
#define NUMBER_FLOAT 3
#define NUMBER_NEGITIVE_FLOAT 4
#define NUMBER_HEX 5
#define NUMBER_BINARY 6

#define TOKEN_ERROR 0
#define TOKEN_PROCEDURE 1
#define TOKEN_ROUTINE 2
#define TOKEN_CALL 3
#define TOKEN_JUMP 4
#define TOKEN_CONDITIONAL 5
#define TOKEN_REGISTER 6
#define TOKEN_ARGUMENT 7
#define TOKEN_USE 8
#define TOKEN_NUMBER 9
#define TOKEN_STRING 10
#define TOKEN_ADDRESS 11
#define TOKEN_TYPE 12
#define TOKEN_ARRAY 13
#define TOKEN_INDEX 14
#define TOKEN_COMPOSITE 15

#define TOKEN_IDENTIFIER 128
#define TOKEN_SIGNATURE_END 130
#define TOKEN_ASSIGNMENT 132
#define TOKEN_EXPRESSION_START 133
#define TOKEN_EXPRESSION_END 134
#define TOKEN_ARRAY_START 135
#define TOKEN_ARRAY_END 136
#define TOKEN_COMPOSITE_START 137
#define TOKEN_COMPOSITE_END 138

#define TOKEN_EOF 255

#define FLAG_INTRENSIC 0x1
#define FLAG_EXTERNAL 0x2

char* token_kind_string[ 256 ] = {
	"error       ",
	"procedure   ",
	"routine     ",
	"call        ",
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
};

typedef struct {
	u64 integer;
	u64 fraction;
	u8 kind;
} number_literal;

typedef struct {
	i32 raw_index;
	i32 child_index;
	i32 child_count;
	u8 bytes;
	u8 kind;
	u8 flags;
/*	u8 padding; */
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

i8 found_start_procedure = 0;
i32 output_llvm_string_count = 0;

pthread_barrier_t thread_sync_barrier;

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

void print_tokens(){
	i32 i = 0;
	while( i < procedure_count ){
		if( procedure[ i ].flags & FLAG_INTRENSIC ){
			printf( "procedure  [%.*s]\n\tintrensic\n\n", procedure[ i ].bytes, &file_buffer[ procedure[ i ].raw_index ]);
		} else {
			printf( "procedure  [%.*s]\n", procedure[ i ].bytes, &file_buffer[ procedure[ i ].raw_index ]);
			i32 j = procedure[ i ].child_index;
			while( j < procedure[ i ].child_index + procedure[ i ].child_count ){
				if( statement[ j ].kind == TOKEN_ROUTINE ){
					printf( "\t%s        %d [%.*s]\n", token_kind_string[ statement[ j ].kind ], statement[ j ].child_count, statement[ j ].bytes, &file_buffer[ statement[ j ].raw_index ]);
				} else {
					printf( "\t\t%s%d [%.*s]\n", token_kind_string[ statement[ j ].kind ], statement[ j ].child_count, statement[ j ].bytes, &file_buffer[ statement[ j ].raw_index ]);
				}
				j++;
			}
		}
		i++;
	}
}

_Atomic i32 found_error = 0;

void compiler_error( syntax_token token, char* message ){
/* absolute hack to prevent multiple threads trying to report errors at the same time. */
	if( found_error == 0 ) found_error = 1; else sleep( 1 );
	printf( "%sError%s %s    token{ .raw_index = %d .child_index = %d .child_count = %d .bytes = %d .kind = %d }\n\n%s\n\n",
	        ANSI_FOREGROUND_RED, ANSI_FOREGROUND_DEFAULT, message, token.raw_index, token.child_index, token.child_count, token.bytes, token.kind, &file_buffer[ token.raw_index ]
	);
	exit( 1 );
}

number_literal token_to_number_literal( syntax_token token ){
	assert( token.kind == TOKEN_NUMBER );
	assert( token.bytes > 0 );
	char* raw_number = &file_buffer[ token.raw_index ];
	i32 raw_index = 0;
	number_literal number = { 0 };
	if( raw_number[ raw_index ] == '0' ){
		raw_index += 1;
		if( token.bytes == 1 ){
/* just 0, do nothing */
		} else if( raw_number[ raw_index ] == 'x' ){
			raw_index += 1;
			number.kind = NUMBER_HEX;
			while( raw_index < token.bytes ){
				number.integer <<= 4;
				if( raw_number[ raw_index ] >= '0' && raw_number[ raw_index ] <= '9' ){
					number.integer |= raw_number[ raw_index ] & 0xf;
				} else if( raw_number[ raw_index ] >= 'a' && raw_number[ raw_index ] <= 'f' ){
					number.integer |= ( raw_number[ raw_index ] & 0xf ) + 9;
				} else {
					assert( raw_number[ raw_index ] == '_' );
				}
				raw_index += 1;
			}
		} else if( raw_number[ raw_index ] == 'b' ){
			raw_index += 1;
			number.kind = NUMBER_BINARY;
			while( raw_index < token.bytes ){
				number.integer <<= 1;
				if( raw_number[ raw_index ] == '1' ){
					number.integer |= 1;
				} else {
					assert( raw_number[ raw_index ] == '0' || raw_number[ raw_index ] == '_' );
				}
				raw_index += 1;
			}
		}
	} else {
		number.kind = NUMBER_INTEGER;
		if( raw_number[ raw_index ] == '-' ){
			assert( token.bytes > 1 );
			raw_index += 1;
			number.kind = NUMBER_NEGITIVE_INTEGER;
		}
		while( raw_index < token.bytes ){
			if( raw_number[ raw_index ] >= '0' && raw_number[ raw_index ] <= '9' ){
				number.integer *= 10;
				number.integer |= raw_number[ raw_index ] & 0xf;
			} else if( raw_number[ raw_index ] == '.' ){
				number.kind = ( number.kind == NUMBER_NEGITIVE_INTEGER ) ? NUMBER_NEGITIVE_FLOAT : NUMBER_FLOAT;
				while( raw_index < token.bytes ){
					if( raw_number[ raw_index ] >= '0' && raw_number[ raw_index ] <= '9' ){
						number.fraction *= 10;
						number.fraction |= raw_number[ raw_index ] & 0xf;
					} else {
						assert( raw_number[ raw_index ] == '_' );
					}
					raw_index += 1;
				}
				assert( raw_index == token.bytes );
				break;
			} else {
				assert( raw_number[ raw_index ] == '_' );
			}
			raw_index += 1;
		}
	}
	return number;
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
		token.raw_index++;
		file->parse++;
		while( file->parse < file_max && *file->parse != '"' ){
			token.bytes++;
			file->parse++;
		}
		if( file->parse == file_max ){
			compiler_error( token, "Unclosed string" );
		}
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
		token.kind = TOKEN_INDEX;
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
		if( token.kind == TOKEN_ARRAY_START ){
			parse_expression( file, token );
		} else if( token.kind == TOKEN_IDENTIFIER ){
			token.kind = TOKEN_COMPOSITE;
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
	syntax_token* type_token = NULL;
	i32 type_start = 0;
	while( token.kind != TOKEN_IDENTIFIER ){
		if( token.kind == TOKEN_ADDRESS ){
			if( type_token == NULL ){
				type_token = push_token( token );
				type_start = scratch_count;
			} else {
				push_token( token );
			}
			token = next_token( file );
		} else if( token.kind == TOKEN_ARRAY_START ){
			token.kind = TOKEN_ARRAY;
			if( type_token == NULL ){
				type_token = push_token( token );
				type_start = scratch_count;
			} else {
				push_token( token );
			}
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
	if( type_token == NULL ){
		type_token = push_token( token );
		type_start = scratch_count;
	} else {
		push_token( token );
	}
	type_token->child_count = scratch_count - type_start;
}

void parse_arguments( source_file* file ){
	assert( file != NULL );
	syntax_token token = next_token( file );
	while( token.kind != TOKEN_SIGNATURE_END ){
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Expected argument name." );
		}
		token.kind = TOKEN_ARGUMENT;
		syntax_token* argument_token = push_token( token );
		i32 argument_start = scratch_count;
		parse_type( file, next_token( file ));
		token = next_token( file );
		argument_token->child_count = scratch_count - argument_start;
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
		if( token.kind != TOKEN_IDENTIFIER ){
			compiler_error( token, "Expected jump location." );
		}
		token.kind = TOKEN_JUMP;
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
					parse_expression( file, next_token( file ));
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
	assert( token != NULL );
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
	return;
}

void validate_is_not_type( syntax_token* token ){
	assert( token != NULL );
	syntax_token* i = type;
	while( i < &type[ type_count ]){
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision, is type." );
		}
		i++;
	}
}

void validate_is_not_global_register( syntax_token* token ){
	assert( token != NULL );
	syntax_token* i = global_register;
	while( i < &global_register[ global_register_count ]){
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision, is global_register." );
		}
		i++;
	}
}

void validate_is_not_procedure( syntax_token* token ){
	assert( token != NULL );
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
	while( i->kind == TOKEN_TYPE || i->kind == TOKEN_ARRAY || i->kind == TOKEN_ADDRESS ){
		i++;
	}
	assert( i->kind == TOKEN_ROUTINE );
	while( i < &statement[ procedure.child_index + procedure.child_count ]){
		assert( i->kind == TOKEN_ROUTINE );
		assert( i->child_count > 0 );
		if( i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision with routine." );
		}
		i += i->child_count + 1;
	}
}

void validate_exclusive_register( syntax_token procedure, syntax_token* token ){
	assert( procedure.kind == TOKEN_PROCEDURE );
	assert( token != NULL );
	assert( token > &statement[ procedure.child_index ]);
	assert( token < &statement[ procedure.child_index + procedure.child_count ]);
	validate_is_not_type( token );
	validate_is_not_global_register( token );
	validate_is_not_procedure( token );
	validate_is_not_routine( procedure, token );
	syntax_token* i = &statement[ procedure.child_index ];
	while( i->kind == TOKEN_TYPE || i->kind == TOKEN_ARRAY || i->kind == TOKEN_ADDRESS ){
		i++;
	}
	assert( i->kind == TOKEN_ROUTINE );
	while( i < token ){
		assert( i->kind == TOKEN_ROUTINE || i->kind == TOKEN_REGISTER || i->kind == TOKEN_ARGUMENT || i->kind == TOKEN_CALL || i->kind == TOKEN_JUMP || i->kind == TOKEN_CONDITIONAL);
		assert( i->child_count > 0 );
		if(( i->kind == TOKEN_ARGUMENT || i->kind == TOKEN_REGISTER ) && i->bytes == token->bytes && memcmp( &file_buffer[ i->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			compiler_error( *token, "Name collision with register." );
		}
		if( i->kind == TOKEN_ROUTINE ){
			i += 1;
		} else {
			i += i->child_count + 1;
		}
	}
}

void validate_expression( syntax_token procedure_token, syntax_token* type_token, syntax_token* token );

void validate_call( syntax_token procedure_token, syntax_token* type_token, syntax_token* token ){
	assert( procedure_token.kind == TOKEN_PROCEDURE );
	assert( token > &statement[ procedure_token.child_index ]);
	assert( token < &statement[ procedure_token.child_index + procedure_token.child_count ]);
	assert( type_token == NULL || type_token->kind == TOKEN_TYPE || type_token->kind == TOKEN_ARRAY || type_token->kind == TOKEN_ADDRESS );
	syntax_token* called_token = &procedure[ 0 ];
	while( 1 ){
		assert( called_token < &procedure[ procedure_count ]);
		if( called_token->bytes == token->bytes && memcmp( &file_buffer[ called_token->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			break;
		}
		called_token++;
		assert( called_token <= &procedure[ procedure_count ]);
		if( called_token == &procedure[ procedure_count ]){
			compiler_error( *token, "Could not find procedure_token name." );
		}
	}
	if( called_token->flags & FLAG_INTRENSIC ){
		if(( called_token->bytes == 3 && memcmp( &file_buffer[ called_token->raw_index ], "add", 3 ) == 0 ) ||
		   ( called_token->bytes == 3 && memcmp( &file_buffer[ called_token->raw_index ], "sub", 3 ) == 0 ) ||
		   ( called_token->bytes == 9 && memcmp( &file_buffer[ called_token->raw_index ], "cmp_equal", 9 ) == 0 )){
			token++;
			validate_expression( procedure_token, type_token, token );
			token += token->child_count + 1;
			validate_expression( procedure_token, type_token, token );
			token += token->child_count + 1;
		} else {
			assert( 0 );
		}
	} else {
		syntax_token* signature_token = &statement[ called_token->child_index ];
		if( type_token == NULL ){
			if( signature_token->kind != TOKEN_ROUTINE ){
				compiler_error( *token, "Call assignment type and procedure return type do not match." );
			}
		} else {
			while( signature_token->kind == TOKEN_ARRAY || signature_token->kind == TOKEN_ADDRESS ){
				if( signature_token->kind == TOKEN_ARRAY && type_token->kind != TOKEN_ARRAY ){
					compiler_error( *token, "Call assignment type and procedure return type do not match." );
				}
				if( signature_token->kind == TOKEN_ADDRESS && type_token->kind != TOKEN_ADDRESS ){
					compiler_error( *token, "Call assignment type and procedure return type do not match." );
				}
				signature_token++;
				type_token++;
			}
			assert( type_token->kind == TOKEN_TYPE );
			if( signature_token->bytes != type_token->bytes || memcmp( &file_buffer[ signature_token->raw_index ], &file_buffer[ type_token->raw_index ], type_token->bytes ) != 0 ){
				compiler_error( *token, "Call assignment type and procedure return type do not match." );
			}
		}
		token++; /* token is currently on the call name, need to move to the first argument. */
		signature_token++;
		signature_token++;
		while( signature_token->kind == TOKEN_ARGUMENT ){
			signature_token++;
			assert( signature_token->kind == TOKEN_TYPE || signature_token->kind == TOKEN_ARRAY || signature_token->kind == TOKEN_ADDRESS );
			validate_expression( procedure_token, signature_token, token );
			signature_token += signature_token->child_count + 1;
			token += token->child_count + 1;
		}
	}
}

void validate_expression( syntax_token procedure_token, syntax_token* type_token, syntax_token* token ){
	assert( procedure_token.kind == TOKEN_PROCEDURE );
	assert( token > &statement[ procedure_token.child_index ]);
	assert( token < &statement[ procedure_token.child_index + procedure_token.child_count ]);
	assert( type_token != NULL );
	assert( type_token->kind == TOKEN_TYPE || type_token->kind == TOKEN_ARRAY || type_token->kind == TOKEN_ADDRESS );
	if( token->kind == TOKEN_NUMBER ){
		number_literal number = token_to_number_literal( *token );
		if( type_token->bytes == 3 && memcmp( &file_buffer[ type_token->raw_index ], "i32", 3 ) == 0 ){
			if( number.kind == NUMBER_FLOAT || number.kind == NUMBER_NEGITIVE_FLOAT ){
				if( number.fraction != 0 ){
					compiler_error( *token, "Type of assignment is not float." );
				}
			}
			if( number.kind == NUMBER_NEGITIVE_INTEGER || number.kind == NUMBER_NEGITIVE_FLOAT ){
				if( number.integer > 2147483648 ){
					compiler_error( *token, "Value is outside of type range." );
				}
			} else {
				if( number.integer >= 4294967296 ){
					compiler_error( *token, "Value is outside of type range." );
				}
			}
		} else if( type_token->bytes == 2 && memcmp( &file_buffer[ type_token->raw_index ], "i8", 2 ) == 0 ){
			if( number.kind == NUMBER_FLOAT || number.kind == NUMBER_NEGITIVE_FLOAT ){
				if( number.fraction != 0 ){
					compiler_error( *token, "Type of assignment is not float." );
				}
			}
			if( number.kind == NUMBER_NEGITIVE_INTEGER || number.kind == NUMBER_NEGITIVE_FLOAT ){
				if( number.integer > 128 ){
					compiler_error( *token, "Value is outside of type range." );
				}
			} else {
				if( number.integer >= 256 ){
					compiler_error( *token, "Value is outside of type range." );
				}
			}
		} else {
			compiler_error( *token, "Invalid type for a number." );
		}
	} else if( token->kind == TOKEN_STRING ){
		if( type_token->kind != TOKEN_ARRAY && type_token->bytes == 2 && memcmp( &file_buffer[ type_token->raw_index ], "i8", 2 ) == 0 ){
			compiler_error( *token, "Invalid type for a string, must be []i8." );
		}
	} else if( token->kind == TOKEN_CALL ){
		validate_call( procedure_token, type_token, token );
	} else if( token->kind == TOKEN_USE || token->kind == TOKEN_INDEX || token->kind == TOKEN_COMPOSITE ){
		assert( file_buffer[ token->raw_index ] != '[' );
/* check match in procedure arguments */
		syntax_token* use_def_token = &statement[ procedure_token.child_index ];
		while( use_def_token->kind == TOKEN_TYPE || use_def_token->kind == TOKEN_ARRAY || use_def_token->kind == TOKEN_ADDRESS ){
			use_def_token += use_def_token->child_count + 1;
		}
		assert( use_def_token->kind == TOKEN_ROUTINE );
		syntax_token* use_def_token_save = use_def_token;
		use_def_token++;
		while( use_def_token->kind == TOKEN_ARGUMENT ){
			if( use_def_token->bytes == token->bytes && memcmp( &file_buffer[ use_def_token->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
				goto found_use_def;
			}
			use_def_token += use_def_token->child_count + 1;
		}
		use_def_token = use_def_token_save;
/* check match in routine arguments */
		syntax_token* end_token = &statement[ procedure_token.child_index + procedure_token.child_count ];
		assert( token > use_def_token );
		while( use_def_token < end_token ){
			if( token <= use_def_token + use_def_token->child_count ){
				assert( token > use_def_token );
				break;
			}
			use_def_token += use_def_token->child_count + 1;
		}
		assert( use_def_token->kind == TOKEN_ROUTINE );
		use_def_token++;
		while( use_def_token->kind == TOKEN_ARGUMENT ){
			if( use_def_token->bytes == token->bytes && memcmp( &file_buffer[ use_def_token->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
				goto found_use_def;
			}
			use_def_token += use_def_token->child_count + 1;
		}
/* check for routine register definitions */
		while( token > use_def_token + use_def_token->child_count ){
			if( use_def_token->bytes == token->bytes && memcmp( &file_buffer[ use_def_token->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
				goto found_use_def;
			}
			use_def_token += use_def_token->child_count + 1;
		}
/* TODO: search globals */
		compiler_error( *token, "Undefinded register use." );
found_use_def:
		use_def_token++;
		assert( use_def_token->kind == TOKEN_TYPE || use_def_token->kind == TOKEN_ARRAY || use_def_token->kind == TOKEN_ADDRESS );
		if( token->kind == TOKEN_USE ){
			while( type_token->kind == TOKEN_ARRAY || type_token->kind == TOKEN_ADDRESS ){
				if( type_token->kind == TOKEN_ARRAY && use_def_token->kind != TOKEN_ARRAY ){
					compiler_error( *token, "register types do not match." );
				}
				if( type_token->kind == TOKEN_ADDRESS && use_def_token->kind != TOKEN_ADDRESS ){
					compiler_error( *token, "register types do not match." );
				}
				type_token++;
				use_def_token++;
			}
			assert( use_def_token->kind == TOKEN_TYPE );
			assert( type_token->kind == TOKEN_TYPE );
			if( type_token->bytes != use_def_token->bytes || memcmp( &file_buffer[ type_token->raw_index ], &file_buffer[ use_def_token->raw_index ], type_token->bytes ) != 0 ){
				compiler_error( *token, "register types do not match." );
			}
		} else if( token->kind == TOKEN_INDEX ){
			if( use_def_token->kind != TOKEN_ARRAY ){
				compiler_error( *token, "Trying to index register that is not an array." );
			}
			use_def_token++;
			while( type_token->kind == TOKEN_ARRAY || type_token->kind == TOKEN_ADDRESS ){
				if( type_token->kind == TOKEN_ARRAY && use_def_token->kind != TOKEN_ARRAY ){
					compiler_error( *token, "register types do not match." );
				}
				if( type_token->kind == TOKEN_ADDRESS && use_def_token->kind != TOKEN_ADDRESS ){
					compiler_error( *token, "register types do not match." );
				}
				type_token++;
				use_def_token++;
			}
			assert( use_def_token->kind == TOKEN_TYPE );
			assert( type_token->kind == TOKEN_TYPE );
			if( type_token->bytes != use_def_token->bytes || memcmp( &file_buffer[ type_token->raw_index ], &file_buffer[ use_def_token->raw_index ], type_token->bytes ) != 0 ){
				compiler_error( *token, "register types do not match." );
			}
		} else if( token->kind == TOKEN_COMPOSITE ){
			if( use_def_token->kind == TOKEN_ARRAY ){
				token++;
				if( token->bytes == 5 && memcmp( &file_buffer[ token->raw_index ], "count", 5 ) == 0 ){
					if( type_token->bytes != 3 || memcmp( &file_buffer[ type_token->raw_index ], "i32", 3 ) != 0 ){
						compiler_error( *token, "Array count is type i32." );
					}
				} else if( token->bytes == 8 && memcmp( &file_buffer[ token->raw_index ], "capacity", 8 ) == 0 ){
					if( type_token->bytes != 3 || memcmp( &file_buffer[ type_token->raw_index ], "i32", 3 ) != 0 ){
						compiler_error( *token, "Array capacity is type i32." );
					}
				} else {
					compiler_error( *token, "Array does not have this memeber." );
				}
			} else if( use_def_token->kind == TOKEN_TYPE ){
				assert( 0 );
			}
		} else {
			assert( 0 );
		}
	} else {
		print_tokens();
		compiler_error( *token, "Invalid expression value" );
	}
}

void validate_jump( syntax_token procedure_token, syntax_token* token ){
	assert( procedure_token.kind == TOKEN_PROCEDURE );
	assert( token != NULL );
	assert( token->kind == TOKEN_JUMP );
	assert( token > &statement[ procedure_token.child_index ]);
	assert( token < &statement[ procedure_token.child_index + procedure_token.child_count ]);
	if( token->bytes == 6 && memcmp( &file_buffer[ token->raw_index ], "return", 6 ) == 0 ){
		syntax_token* return_type_token = &statement[ procedure_token.child_index ];
		token++;
		while( return_type_token->kind == TOKEN_TYPE || return_type_token->kind == TOKEN_ARRAY || return_type_token->kind == TOKEN_ADDRESS ){
			validate_expression( procedure_token, return_type_token, token );
			return_type_token += return_type_token->child_count + 1;
			token += token->child_count + 1;
		}
	} else {
		syntax_token* routine_token = &statement[ procedure_token.child_index ];
		while( routine_token->kind == TOKEN_TYPE || routine_token->kind == TOKEN_ARRAY || routine_token->kind == TOKEN_ADDRESS ){
			routine_token += routine_token->child_count + 1;
		}
		assert( routine_token->kind == TOKEN_ROUTINE );
		assert( routine_token < &statement[ procedure_token.child_index + procedure_token.child_count ]);
		while( routine_token < &statement[ procedure_token.child_index + procedure_token.child_count ]){
			assert( routine_token->kind == TOKEN_ROUTINE );
			assert( routine_token->child_count > 0 );
			if( routine_token->bytes == token->bytes && memcmp( &file_buffer[ routine_token->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
				routine_token++;
				token++;
				while( routine_token->kind == TOKEN_ARGUMENT ){
					routine_token++;
					assert( routine_token->kind == TOKEN_TYPE || routine_token->kind == TOKEN_ARRAY || routine_token->kind == TOKEN_ADDRESS );
					validate_expression( procedure_token, routine_token, token );
					routine_token += routine_token->child_count + 1;
					token += token->child_count + 1;
				}
				break;
			}
			routine_token += routine_token->child_count + 1;
			if( routine_token >= &statement[ procedure_token.child_index + procedure_token.child_count ]){
				compiler_error( *token, "jump point not found." );
			}
		}
	}
}

void validate_procedure( i32 procedure_index ){
	assert( procedure_index >= 0 );
	assert( procedure_index < procedure_count );
	if( procedure[ procedure_index ].flags & FLAG_INTRENSIC ){
		return;
	}
	if( procedure[ procedure_index ].flags & FLAG_EXTERNAL ){
		return;
	}
	{
/* validate exclusive procedure */
		syntax_token token = procedure[ procedure_index ];
		if( found_start_procedure == 0 && token.bytes == 5 && memcmp( &file_buffer[ token.raw_index ], "start", 5 ) == 0 ){
			found_start_procedure = 1;
		}
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
	syntax_token* procedure_start = token;
	syntax_token* end_token = &statement[ procedure[ procedure_index ].child_count + procedure[ procedure_index ].child_index ];
	while( token < end_token ){
		assert( token->kind == TOKEN_ROUTINE );
		validate_is_not_type( token );
/* validate exclusive routine */
		syntax_token* routine_token = procedure_start;
		assert( routine_token->kind == TOKEN_ROUTINE );
		while( routine_token < token ){
			assert( routine_token->kind == TOKEN_ROUTINE );
			assert( routine_token->child_count > 0 );
			if( routine_token->bytes == token->bytes && memcmp( &file_buffer[ routine_token->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
				compiler_error( *token, "Routine name collision." );
			}
			routine_token += routine_token->child_count + 1;
		}
		token++;
/* validate routine arguments */
		while( token->kind == TOKEN_ARGUMENT ){
			validate_exclusive_register( procedure[ procedure_index ], token );
			token++;
			validate_is_type( token );
			token += token->child_count + 1;
		}
/* validate routine body */
		while( token->kind == TOKEN_REGISTER || token->kind == TOKEN_CALL ){
			if( token->kind == TOKEN_REGISTER ){
				syntax_token* type_token = token + 1;
				validate_exclusive_register( procedure[ procedure_index ], token );
				token++;
				validate_is_type( token );
				token += token->child_count + 1;
				validate_expression( procedure[ procedure_index ], type_token, token );
				token += token->child_count + 1;
			} else if( token->kind == TOKEN_CALL ){
				validate_call( procedure[ procedure_index ], NULL, token );
				token += token->child_count + 1;
			}
		}
/* validate routine end */
		if( token->kind == TOKEN_JUMP ){
			validate_jump( procedure[ procedure_index ], token );
			token += token->child_count + 1;
		} else if( token->kind == TOKEN_CONDITIONAL ){
			token++;
			validate_expression( procedure[ procedure_index ], &type[ 0 ], token );
			token += token->child_count + 1;
			validate_jump( procedure[ procedure_index ], token );
			token += token->child_count + 1;
			validate_jump( procedure[ procedure_index ], token );
			token += token->child_count + 1;
		} else {
			assert( 0 );
		}
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
		i++;
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
		i++;
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

void output_append( char* data, i32 bytes ){
	buffer_append( (char*) scratch, &scratch_count, data, bytes );
}

void output_insert( i32* index, char* data, i32 bytes ){
	buffer_insert( (char*) scratch, &scratch_count, *index, data, bytes );
	*index += bytes;
}

i32 string_token_length( syntax_token token ){
	assert( token.kind == TOKEN_STRING );
	i32 i = 0;
	i32 string_bytes = 0;
	while( i < token.bytes ){
		if( file_buffer[ token.raw_index + i ] == '\\' ){
			i += 3;
		} else {
			i += 1;
		}
		string_bytes += 1;
	}
	return string_bytes;
}

i32 output_llvm_call( syntax_token* token, syntax_token* type_token, i32 call_index, i32 statement_start_index, i32* statement_nesting ){
	assert( token != NULL );
	assert( token->kind == TOKEN_CALL );
	if( type_token != NULL ){
		assert( type_token->kind == TOKEN_TYPE );
	}
	assert( call_index >= 0 );
	assert( call_index <= scratch_count );
	assert( statement_start_index >= 0 );
	assert( statement_start_index <= call_index );
	assert( statement_nesting != NULL );
	assert( *statement_nesting >= 0 );
	syntax_token* call_def = procedure;
	while( 1 ){
		if( call_def->bytes == token->bytes && memcmp( &file_buffer[ call_def->raw_index ], &file_buffer[ token->raw_index ], token->bytes ) == 0 ){
			break;
		}
		call_def++;
		assert( call_def < &procedure[ procedure_count - 1 ] );
	}
	i32 output_index = call_index;
	if( call_def->flags & FLAG_INTRENSIC ){
		output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
		output_insert( &output_index, " ", 1 );
		output_insert( &output_index, &file_buffer[ type_token->raw_index ], type_token->bytes );
		token++;
		if( token->kind == TOKEN_NUMBER ){
			output_insert( &output_index, " ", 1 );
			output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
			token++;
		} else if( token->kind == TOKEN_USE ){
			output_insert( &output_index, " %", 2 );
			output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
			token++;
		} else if( token->kind == TOKEN_CALL ){
			i32 nested_call_index = statement_start_index;
			char buffer[ 64 ];
			i32 nest_postfix_length = sprintf( buffer, ".%d", *statement_nesting );
			*statement_nesting += 1;
			output_insert( &nested_call_index, "\t%", 2 );
			output_insert( &nested_call_index, &file_buffer[ token->raw_index ], token->bytes );
			output_insert( &nested_call_index, buffer, nest_postfix_length );
			output_insert( &nested_call_index, " = ", 3 );
			output_index += nested_call_index - statement_start_index;
			output_index += output_llvm_call( token, type_token, nested_call_index, statement_start_index, statement_nesting );
			output_insert( &output_index, " %", 2 );
			output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
			output_insert( &output_index, buffer, nest_postfix_length );
			token += token->child_count + 1;
		} else {
			assert( 0 );
		}
		if( token->kind == TOKEN_NUMBER ){
			output_insert( &output_index, ", ", 2 );
			output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
		} else if( token->kind == TOKEN_USE ){
			output_insert( &output_index, ", %", 3 );
			output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
		} else if( token->kind == TOKEN_CALL ){
			i32 nested_call_index = statement_start_index;
			char buffer[ 64 ];
			i32 nest_postfix_length = sprintf( buffer, ".%d", *statement_nesting );
			*statement_nesting += 1;
			output_insert( &nested_call_index, "\t%", 2 );
			output_insert( &nested_call_index, &file_buffer[ token->raw_index ], token->bytes );
			output_insert( &nested_call_index, buffer, nest_postfix_length );
			output_insert( &nested_call_index, " = ", 3 );
			output_index += nested_call_index - statement_start_index;
			output_index += output_llvm_call( token, type_token, nested_call_index, statement_start_index, statement_nesting );
			output_insert( &output_index, ", %", 3 );
			output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
			output_insert( &output_index, buffer, nest_postfix_length );
			token += token->child_count + 1;
		} else {
			assert( 0 );
		}
		output_insert( &output_index, "\n", 1 );
	} else {
		syntax_token* call_argument = &statement[ call_def->child_index ];
		if( call_argument->kind == TOKEN_ROUTINE ){
			output_insert( &output_index, "\tcall void @", 12 );
			call_argument++;
		} else if( call_argument->kind == TOKEN_TYPE ){
			output_insert( &output_index, "call ", 5 );
			output_insert( &output_index, &file_buffer[ call_argument->raw_index ], call_argument->bytes );
			output_insert( &output_index, " @", 2 );
			call_argument++;
			assert( call_argument->kind == TOKEN_ROUTINE );
			call_argument++;
		} else {
			assert( 0 );
		}
		output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
		output_insert( &output_index, "( ", 2 );
		token++;
		while( call_argument->kind == TOKEN_ARGUMENT ){
			call_argument++;
			if( call_argument->kind == TOKEN_TYPE ){
				output_insert( &output_index, &file_buffer[ call_argument->raw_index ], call_argument->bytes );
				output_insert( &output_index, " ", 1 );
				if( token->kind == TOKEN_NUMBER ){
					output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
					call_argument += call_argument->child_count + 1;
					token += token->child_count + 1;
				} else if( token->kind == TOKEN_USE ){
					output_insert( &output_index, "%", 1 );
					output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
					call_argument += call_argument->child_count + 1;
					token += token->child_count + 1;
				} else {
					assert( 0 );
				}
			} else if( call_argument->kind == TOKEN_ARRAY ){
				call_argument++;
				if( token->kind == TOKEN_STRING ){
					assert( call_argument->kind == TOKEN_TYPE && call_argument->bytes == 2 && memcmp( &file_buffer[ call_argument->raw_index ], "i8", 2 ) == 0 );
					i32 string_index = 0;
					i32 llvm_output_string_index = __atomic_fetch_add( &output_llvm_string_count, 1, __ATOMIC_ACQ_REL );
					char string_name_buffer[ 64 ];
					i32 string_name_buffer_count = sprintf( string_name_buffer, "@.string.%d", llvm_output_string_index );
					i32 string_bytes = string_token_length( *token );
					char string_bytes_buffer[ 64 ];
					i32 string_bytes_buffer_count = sprintf( string_bytes_buffer, "%d", string_bytes );
					output_insert( &string_index, string_name_buffer, string_name_buffer_count );
					output_insert( &string_index, " = constant [", 13 );
					output_insert( &string_index, string_bytes_buffer, string_bytes_buffer_count );
					output_insert( &string_index, " x i8] c\"", 9 );
					output_insert( &string_index, &file_buffer[ token->raw_index ], token->bytes );
					output_insert( &string_index, "\"\n\n", 3 );
					output_index += string_index;
					output_insert( &output_index, "i32 ", 4 );
					output_insert( &output_index, string_bytes_buffer, string_bytes_buffer_count );
					output_insert( &output_index, ", i32 ", 6 );
					output_insert( &output_index, string_bytes_buffer, string_bytes_buffer_count );
					output_insert( &output_index, ", ptr ", 6 );
					output_insert( &output_index, string_name_buffer, string_name_buffer_count );
				} else if( token->kind == TOKEN_USE ){
					output_insert( &output_index, "i32 %", 5 );
					output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
					output_insert( &output_index, ".count, i32 %", 13 );
					output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
					output_insert( &output_index, ".capacity, ptr %", 16 );
					output_insert( &output_index, &file_buffer[ token->raw_index ], token->bytes );
					output_insert( &output_index, ".data", 5 );
				} else {
					assert( 0 );
				}
			} else {
				assert( 0 );
			}
		}
		output_insert( &output_index, " )\n", 3 );
	}
	return output_index - call_index;
}

void output_llvm_procedure( i32 procedure_index ){
	assert( procedure_index >= 0 );
	syntax_token* procedure_token = &procedure[ procedure_index ];
	assert( scratch_count == 0 );
	assert( !( procedure_token->flags & FLAG_INTRENSIC ));
	if( procedure_token->flags & FLAG_EXTERNAL ){
		output_append( "declare ", 8 );
		syntax_token* token = &statement[ procedure_token->child_index ];
		if( token->kind == TOKEN_ROUTINE ){
			output_append( "void @", 6 );
		} else {
			output_append( &file_buffer[ token->raw_index ], token->bytes );
			token++;
			output_append( " @", 2 );
		}
		output_append( &file_buffer[ procedure_token->raw_index ], procedure_token->bytes );
		output_append( "( ", 2 );
		assert( token->kind == TOKEN_ROUTINE );
		token++;
		while( token->kind == TOKEN_ARGUMENT ){
			token++;
			if( token->kind == TOKEN_TYPE ){
				output_append( &file_buffer[ token->raw_index ], token->bytes );
			} else if( token->kind == TOKEN_ADDRESS ){
				output_append( "ptr", 3 );
			} else if( token->kind == TOKEN_ARRAY ){
				output_append( "i32, i32, ptr", 13 );
			}
			token += token->child_count + 1;
		}
		output_append( " )\n\n", 4 );
	} else {
		output_append( "define ", 7 );
		syntax_token* token = &statement[ procedure_token->child_index ];
		assert( token->kind == TOKEN_TYPE );
		output_append( &file_buffer[ token->raw_index ], token->bytes );
		token++;
		output_append( " @", 2 );
		output_append( &file_buffer[ procedure_token->raw_index ], procedure_token->bytes );
		output_append( "( ", 2 );
		assert( token->kind == TOKEN_ROUTINE );
		token++;
		while( token->kind == TOKEN_ARGUMENT ){
			if( ( token - 1 )->kind == TOKEN_ARGUMENT ){
				output_append( ", ", 2 );
			}
			syntax_token* argument_token = token;
			token++;
			if( token->kind == TOKEN_TYPE ){
				output_append( &file_buffer[ token->raw_index ], token->bytes );
				output_append( " %", 2 );
				output_append( &file_buffer[ argument_token->raw_index ], argument_token->bytes );
			} else if( token->kind == TOKEN_ADDRESS ){
				output_append( "ptr %", 5 );
				output_append( &file_buffer[ argument_token->raw_index ], argument_token->bytes );
			} else if( token->kind == TOKEN_ARRAY ){
				output_append( "i32 %", 5 );
				output_append( &file_buffer[ argument_token->raw_index ], argument_token->bytes );
				output_append( ".count, i32 %", 13 );
				output_append( &file_buffer[ argument_token->raw_index ], argument_token->bytes );
				output_append( ".capacity, ptr %", 16 );
				output_append( &file_buffer[ argument_token->raw_index ], argument_token->bytes );
				output_append( ".data", 5 );
			}
			while( token->kind == TOKEN_TYPE || token->kind == TOKEN_ADDRESS || token->kind == TOKEN_ARRAY ){
				token++;
			}
		}
		output_append( " ){\n", 4 );
		i32 statement_nesting = 0;
		assert( token < &statement[ procedure_token->child_index + procedure_token->child_count ]);
		while( token < &statement[ procedure_token->child_index + procedure_token->child_count ]){
			while( token->kind == TOKEN_REGISTER || token->kind == TOKEN_CALL ){
				i32 statement_start_index = scratch_count;
				if( token->kind == TOKEN_REGISTER ){
					output_append( "\t%", 2 );
					output_append( &file_buffer[ token->raw_index ], token->bytes );
					token++;
					if( token->kind == TOKEN_TYPE ){
						syntax_token* type_token = token;
						token++;
						if( token->kind == TOKEN_NUMBER ){
							output_append( " = add ", 7 );
							output_append( &file_buffer[ type_token->raw_index ], type_token->bytes );
							output_append( " 0, ", 4 );
							output_append( &file_buffer[ token->raw_index ], token->bytes );
							output_append( "\n", 1 );
							token++;
						} else if( token->kind == TOKEN_USE ){
							output_append( " = add ", 7 );
							output_append( &file_buffer[ type_token->raw_index ], type_token->bytes );
							output_append( " 0, %", 5 );
							output_append( &file_buffer[ token->raw_index ], token->bytes );
							output_append( "\n", 1 );
							token++;
						} else if( token->kind == TOKEN_CALL ){
							output_append( " = ", 3 );
							output_llvm_call( token, type_token, scratch_count, statement_start_index, &statement_nesting );
							token += token->child_count + 1;
						} else {
							assert( 0 );
						}
					} else if( token->kind == TOKEN_ARRAY ){
						syntax_token* statement_token = token - 1;
						token++;
						assert( token->kind == TOKEN_TYPE && token->bytes == 2 && memcmp( &file_buffer[ token->raw_index ], "i8", 2 ) == 0 );
						token++;
						assert( token->kind == TOKEN_STRING );
						i32 string_index = 0;
						i32 llvm_output_string_index = __atomic_fetch_add( &output_llvm_string_count, 1, __ATOMIC_ACQ_REL );
						char string_name_buffer[ 64 ];
						i32 string_name_buffer_count = sprintf( string_name_buffer, "@.string.%d", llvm_output_string_index );
						i32 string_bytes = string_token_length( *token );
						char string_bytes_buffer[ 64 ];
						i32 string_bytes_buffer_count = sprintf( string_bytes_buffer, "%d", string_bytes );
						output_insert( &string_index, string_name_buffer, string_name_buffer_count );
						output_insert( &string_index, " = constant [", 13 );
						output_insert( &string_index, string_bytes_buffer, string_bytes_buffer_count );
						output_insert( &string_index, " x i8] c\"", 9 );
						output_insert( &string_index, &file_buffer[ token->raw_index ], token->bytes );
						output_insert( &string_index, "\"\n\n", 3 );
						output_append( ".count = add i32 0, ", 20 );
						output_append( string_bytes_buffer, string_bytes_buffer_count );
						token = statement_token;
						output_append( "\n\t%", 3 );
						output_append( &file_buffer[ token->raw_index ], token->bytes );
						output_append( ".capacity = add i32 0, ", 23 );
						output_append( string_bytes_buffer, string_bytes_buffer_count );
						token = statement_token;
						output_append( "\n\t%", 3 );
						output_append( &file_buffer[ token->raw_index ], token->bytes );
						output_append( ".data = getelementptr ptr, ptr ", 31 );
						output_append( string_name_buffer, string_name_buffer_count );
						output_append( "\n", 1 );
						token += 4;
					} else {
						assert( 0 );
					}
				} else if( token->kind == TOKEN_CALL ){
					output_llvm_call( token, NULL, scratch_count, statement_start_index, &statement_nesting );
					token += token->child_count + 1;
				} else {
					assert( 0 );
				}
			}
			assert( token->kind == TOKEN_CONDITIONAL || token->kind == TOKEN_JUMP );
			if( token->kind == TOKEN_JUMP ){
				i32 statement_start_index = scratch_count;
				if( token->bytes == 6 && memcmp( &file_buffer[ token->raw_index ], "return", 6 ) == 0 ){
					output_append( "\tret ", 5 );
					output_append( &file_buffer[ statement[ procedure_token->child_index ].raw_index ], statement[ procedure_token->child_index ].bytes );
					output_append( " ", 1 );
					token++;
					if( token->kind == TOKEN_NUMBER ){
						output_append( &file_buffer[ token->raw_index ], token->bytes );
					} else if( token->kind == TOKEN_USE ){
						output_append( "%", 1 );
						output_append( &file_buffer[ token->raw_index ], token->bytes );
					} else if( token->kind == TOKEN_COMPOSITE ){
						output_append( "%", 1 );
						output_append( &file_buffer[ token->raw_index ], token->bytes );
						token++;
						output_append( ".", 1 );
						output_append( &file_buffer[ token->raw_index ], token->bytes );
					} else if( token->kind == TOKEN_CALL ){
						i32 nested_call_index = statement_start_index;
						char buffer[ 64 ];
						i32 nest_postfix_length = sprintf( buffer, ".%d", statement_nesting );
						statement_nesting += 1;
						output_insert( &nested_call_index, "\t%", 2 );
						output_insert( &nested_call_index, &file_buffer[ token->raw_index ], token->bytes );
						output_insert( &nested_call_index, buffer, nest_postfix_length );
						output_insert( &nested_call_index, " = ", 3 );
						output_llvm_call( token, &statement[ procedure_token->child_index ], nested_call_index, statement_start_index, &statement_nesting );
						output_append( "%", 1 );
						output_append( &file_buffer[ token->raw_index ], token->bytes );
						output_append( buffer, nest_postfix_length );
						token += token->child_count + 1;
					} else if( token->kind == TOKEN_STRING ){
						assert( 0 && "string" );
					} else if( token->kind == TOKEN_INDEX ){
						assert( 0 && "array" );
					} else {
						assert( 0 );
					}
					token += token->child_count + 1;
					output_append( "\n", 1 );
				} else {
					assert( 0 );
				}
			} else if( token->kind == TOKEN_CONDITIONAL ){
				assert( 0 );
			} else {
				assert( 0 );
			}
			break;
		}
		output_append( "}\n\n", 3 );
	}
	push_output();
}

void output_procedure( i32 index ){
	assert( index >= 0 );
	syntax_token* procedure_token = &procedure[ index ];
	if( procedure_token->flags & FLAG_INTRENSIC ){
		return;
	}
	output_llvm_procedure( index );
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

void thread_sync(){
#ifndef SINGLE_THREAD
	pthread_barrier_wait( &thread_sync_barrier );
#endif
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
	thread_sync();
	if( thread_index == 0 && !found_start_procedure ){
		printf( "%sError%s 'start' procedure not found.", ANSI_FOREGROUND_RED, ANSI_FOREGROUND_DEFAULT );
		exit( 1 );
	}
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
	thread_sync();
	if( thread_index == 0 ){
		char* target = "target triple = \"x86_64-unknown-linux-gnu\"\n\n";
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
		error( "Usage: ssal_llvm <filename>" );
	}
	pthread_mutex_init( &file_mutex, NULL );
	pthread_mutex_init( &procedure_mutex, NULL );
	pthread_mutex_init( &type_mutex, NULL );
	pthread_mutex_init( &global_register_mutex, NULL );
	pthread_mutex_init( &output_mutex, NULL );
	{
		type[ type_count ].raw_index = file_buffer_count;
		type[ type_count ].bytes = 3;
		buffer_append( file_buffer, &file_buffer_count, "i32", type[ type_count ].bytes );
		type[ type_count ].child_index = -1;
		type[ type_count ].child_count = 0;
		type[ type_count ].kind = TOKEN_TYPE;
		type[ type_count ].flags = FLAG_INTRENSIC;
		type_count++;
		type[ type_count ].raw_index = file_buffer_count;
		type[ type_count ].bytes = 2;
		buffer_append( file_buffer, &file_buffer_count, "i8", type[ type_count ].bytes );
		type[ type_count ].child_index = -1;
		type[ type_count ].child_count = 0;
		type[ type_count ].kind = TOKEN_TYPE;
		type[ type_count ].flags = FLAG_INTRENSIC;
		type_count++;
		procedure[ procedure_count ].raw_index = file_buffer_count;
		procedure[ procedure_count ].bytes = 3;
		buffer_append( file_buffer, &file_buffer_count, "add", procedure[ procedure_count ].bytes );
		procedure[ procedure_count ].child_index = -1;
		procedure[ procedure_count ].child_count = 0;
		procedure[ procedure_count ].kind = TOKEN_PROCEDURE;
		procedure[ procedure_count ].flags = FLAG_INTRENSIC;
		procedure_count++;
		procedure[ procedure_count ].raw_index = file_buffer_count;
		procedure[ procedure_count ].bytes = 3;
		buffer_append( file_buffer, &file_buffer_count, "sub", procedure[ procedure_count ].bytes );
		procedure[ procedure_count ].child_index = -1;
		procedure[ procedure_count ].child_count = 0;
		procedure[ procedure_count ].kind = TOKEN_PROCEDURE;
		procedure[ procedure_count ].flags = FLAG_INTRENSIC;
		procedure_count++;
		procedure[ procedure_count ].raw_index = file_buffer_count;
		procedure[ procedure_count ].bytes = 9;
		buffer_append( file_buffer, &file_buffer_count, "cmp_equal", procedure[ procedure_count ].bytes );
		procedure[ procedure_count ].child_index = -1;
		procedure[ procedure_count ].child_count = 0;
		procedure[ procedure_count ].kind = TOKEN_PROCEDURE;
		procedure[ procedure_count ].flags = FLAG_INTRENSIC;
		procedure_count++;
		procedure[ procedure_count ].raw_index = file_buffer_count;
		procedure[ procedure_count ].bytes = 4;
		buffer_append( file_buffer, &file_buffer_count, "exit", procedure[ procedure_count ].bytes );
		procedure[ procedure_count ].child_index = statement_count;
		procedure[ procedure_count ].child_count = 3;
		procedure[ procedure_count ].kind = TOKEN_PROCEDURE;
		procedure[ procedure_count ].flags = FLAG_EXTERNAL;
		procedure_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 9;
			buffer_append( file_buffer, &file_buffer_count, "procedure", statement[ statement_count ].bytes );
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 2;
			statement[ statement_count ].kind = TOKEN_ROUTINE;
			statement[ statement_count ].flags = 0;
			statement_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 0;
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 1;
			statement[ statement_count ].kind = TOKEN_ARGUMENT;
			statement[ statement_count ].flags = 0;
			statement_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 3;
			buffer_append( file_buffer, &file_buffer_count, "i32", statement[ statement_count ].bytes );
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 0;
			statement[ statement_count ].kind = TOKEN_TYPE;
			statement[ statement_count ].flags = 0;
			statement_count++;
		procedure[ procedure_count ].raw_index = file_buffer_count;
		procedure[ procedure_count ].bytes = 3;
		buffer_append( file_buffer, &file_buffer_count, "log", procedure[ procedure_count ].bytes );
		procedure[ procedure_count ].child_index = statement_count;
		procedure[ procedure_count ].child_count = 5;
		procedure[ procedure_count ].kind = TOKEN_PROCEDURE;
		procedure[ procedure_count ].flags = FLAG_EXTERNAL;
		procedure_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 3;
			buffer_append( file_buffer, &file_buffer_count, "i32", statement[ statement_count ].bytes );
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 0;
			statement[ statement_count ].kind = TOKEN_TYPE;
			statement[ statement_count ].flags = 0;
			statement_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 9;
			buffer_append( file_buffer, &file_buffer_count, "procedure", statement[ statement_count ].bytes );
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 3;
			statement[ statement_count ].kind = TOKEN_ROUTINE;
			statement[ statement_count ].flags = 0;
			statement_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 0;
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 2;
			statement[ statement_count ].kind = TOKEN_ARGUMENT;
			statement[ statement_count ].flags = 0;
			statement_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 1;
			buffer_append( file_buffer, &file_buffer_count, "[", statement[ statement_count ].bytes );
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 1;
			statement[ statement_count ].kind = TOKEN_ARRAY;
			statement[ statement_count ].flags = 0;
			statement_count++;
			statement[ statement_count ].raw_index = file_buffer_count;
			statement[ statement_count ].bytes = 2;
			buffer_append( file_buffer, &file_buffer_count, "i8", statement[ statement_count ].bytes );
			statement[ statement_count ].child_index = 0;
			statement[ statement_count ].child_count = 0;
			statement[ statement_count ].kind = TOKEN_TYPE;
			statement[ statement_count ].flags = 0;
			statement_count++;
	}
	i32 name_bytes = strlen( argv[ 1 ]);
	file_add( argv[ 1 ], name_bytes );
#ifdef SINGLE_THREAD
	thread_main( (void*) ( 0 ));
#else /* default: use multiple threads */
	i64 cpu_count = sysconf( _SC_NPROCESSORS_ONLN );
	assert( cpu_count > 0 );
	pthread_barrier_init( &thread_sync_barrier, NULL, cpu_count );
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
